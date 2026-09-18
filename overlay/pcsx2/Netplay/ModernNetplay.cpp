// SPDX-FileCopyrightText: 2026 PCSX2 Modern Netplay Port contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Netplay/ModernNetplay.h"

#include "Config.h"
#include "Counters.h"
#include "GameList.h"
#include "Host.h"
#include "VMManager.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Memcard/MemoryCardFolder.h"
#include "SIO/Pad/Pad.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace ModernNetplay
{
#ifdef _WIN32
namespace
{
    static constexpr std::size_t INPUT_FRAME_BYTES = 18;
    using InputFrame = std::array<std::uint8_t, INPUT_FRAME_BYTES>;
    using InputBundle = std::array<InputFrame, MAX_PLAYERS>;

    constexpr std::uint32_t HELLO_MAGIC = 0x50324E50;   // P2NP
    constexpr std::uint32_t INPUT_MAGIC = 0x494E5054;   // INPT
    constexpr std::uint32_t BUNDLE_MAGIC = 0x424E444C;  // BNDL
    constexpr std::uint32_t CONTROL_MAGIC = 0x43544C31; // CTL1
    constexpr std::uint32_t PROTOCOL_VERSION = 6;
    constexpr std::uint16_t DEFAULT_PORT = 27886;
    constexpr int RECEIVE_TIMEOUT_SECONDS = 30;
    constexpr int BOOT_BARRIER_TIMEOUT_SECONDS = 90;
    constexpr std::uint32_t LOG_FRAME_INTERVAL = 120;
    constexpr std::uint32_t MAX_CONTROL_PAYLOAD = 64 * 1024;
    constexpr std::uint32_t MEMCARD_CHUNK = 60 * 1024;
    constexpr std::size_t HELLO_SIZE = 80;
    constexpr std::size_t INPUT_PACKET_SIZE = 16 + INPUT_FRAME_BYTES;
    constexpr std::size_t INPUT_REST_SIZE = 12 + INPUT_FRAME_BYTES;
    constexpr std::size_t BUNDLE_PACKET_SIZE = 16 + (MAX_PLAYERS * INPUT_FRAME_BYTES);
    constexpr std::size_t BUNDLE_REST_SIZE = 12 + (MAX_PLAYERS * INPUT_FRAME_BYTES);
    constexpr std::size_t RUNTIME_CONFIG_SIZE = 28;
    constexpr InputFrame NEUTRAL_FRAME = {
        0xff, 0xff,                   // digital buttons
        0x7f, 0x7f, 0x7f, 0x7f,     // right/left analog axes
        0x00, 0x00, 0x00, 0x00,     // d-pad pressure
        0x00, 0x00, 0x00, 0x00,     // triangle/circle/cross/square pressure
        0x00, 0x00, 0x00, 0x00      // L1/R1/L2/R2 pressure
    };

    enum class Role : std::uint32_t
    {
        Disabled = 0,
        Host = 1,
        Client = 2,
    };

    enum class ControlType : std::uint32_t
    {
        Roster = 1,
        GameManifest = 2,
        GameMatch = 3,
        MemcardBegin = 4,
        MemcardChunk = 5,
        MemcardEnd = 6,
        MemcardReady = 7,
        PrepareBoot = 8,
        BootReady = 9,
        StartCommit = 10,
        FirstPollReady = 11,
        FirstPollGo = 12,
        SessionAbort = 13,
        RuntimeRequest = 14,
        RuntimeApply = 15,
        RuntimeAck = 16,
        RuntimeCommit = 17,
    };

    struct PlayerState
    {
        bool connected = false;
        bool game_match = false;
        bool memcard_ready = false;
        bool boot_ready = false;
        bool first_poll_ready = false;
        std::uint32_t controller = 0;
        std::string name;
    };

    struct Peer
    {
        SOCKET socket = INVALID_SOCKET;
        std::uint32_t player_id = 0;
        std::string name;
        std::mutex send_mutex;
        std::thread receiver;
        std::atomic<bool> connected{false};
    };

    void WriteU32(std::uint8_t* dst, std::uint32_t value)
    {
        value = htonl(value);
        std::memcpy(dst, &value, sizeof(value));
    }

    std::uint32_t ReadU32(const std::uint8_t* src)
    {
        std::uint32_t value;
        std::memcpy(&value, src, sizeof(value));
        return ntohl(value);
    }

    void WriteU64(std::uint8_t* dst, std::uint64_t value)
    {
        WriteU32(dst, static_cast<std::uint32_t>(value >> 32));
        WriteU32(dst + 4, static_cast<std::uint32_t>(value & 0xffffffffu));
    }

    std::uint64_t ReadU64(const std::uint8_t* src)
    {
        return (static_cast<std::uint64_t>(ReadU32(src)) << 32) |
            static_cast<std::uint64_t>(ReadU32(src + 4));
    }

    std::uint64_t HashBytes(const std::vector<std::uint8_t>& data)
    {
        std::uint64_t hash = 1469598103934665603ull;
        for (const std::uint8_t value : data)
        {
            hash ^= value;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    bool SendAll(SOCKET socket, const void* data, std::size_t size)
    {
        const char* ptr = static_cast<const char*>(data);
        while (size > 0)
        {
            const int sent = send(socket, ptr, static_cast<int>(std::min<std::size_t>(size, 0x7fffffff)), 0);
            if (sent <= 0)
                return false;
            ptr += sent;
            size -= static_cast<std::size_t>(sent);
        }
        return true;
    }

    bool ReceiveAll(SOCKET socket, void* data, std::size_t size)
    {
        char* ptr = static_cast<char*>(data);
        while (size > 0)
        {
            const int received = recv(socket, ptr, static_cast<int>(std::min<std::size_t>(size, 0x7fffffff)), 0);
            if (received <= 0)
                return false;
            ptr += received;
            size -= static_cast<std::size_t>(received);
        }
        return true;
    }

    bool ConnectWithTimeout(SOCKET socket, const sockaddr* address, int address_length, int timeout_ms)
    {
        u_long nonblocking = 1;
        if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0)
            return false;

        int result = connect(socket, address, address_length);
        if (result == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEINVAL)
            {
                nonblocking = 0;
                ioctlsocket(socket, FIONBIO, &nonblocking);
                return false;
            }

            fd_set write_set;
            fd_set error_set;
            FD_ZERO(&write_set);
            FD_ZERO(&error_set);
            FD_SET(socket, &write_set);
            FD_SET(socket, &error_set);
            timeval timeout{};
            timeout.tv_sec = timeout_ms / 1000;
            timeout.tv_usec = (timeout_ms % 1000) * 1000;
            result = select(0, nullptr, &write_set, &error_set, &timeout);
            if (result <= 0 || FD_ISSET(socket, &error_set))
            {
                nonblocking = 0;
                ioctlsocket(socket, FIONBIO, &nonblocking);
                return false;
            }

            int socket_error = 0;
            int socket_error_size = sizeof(socket_error);
            if (getsockopt(socket, SOL_SOCKET, SO_ERROR,
                    reinterpret_cast<char*>(&socket_error), &socket_error_size) != 0 ||
                socket_error != 0)
            {
                nonblocking = 0;
                ioctlsocket(socket, FIONBIO, &nonblocking);
                return false;
            }
        }

        nonblocking = 0;
        return ioctlsocket(socket, FIONBIO, &nonblocking) == 0;
    }

    std::string TruncateUtf8(const std::string& value, std::size_t max_bytes)
    {
        if (value.size() <= max_bytes)
            return value;

        std::size_t cut = max_bytes;
        while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0u) == 0x80u)
            --cut;
        return value.substr(0, cut);
    }

    class Session final
    {
    public:
        Session()
        {
            ReadConfiguration();
            if (m_role == Role::Disabled)
                return;

            m_session_id = static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
            for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
                m_players[i].controller = i + 1;
            m_players[0].connected = (m_role == Role::Host);
            m_players[0].name = (m_role == Role::Host) ? m_username : std::string("房主");
            if (m_role == Role::Host)
                m_local_player_id = 1;

            OpenLog();
            Log("session configured: role=%s user=%s port=%u players=%u delay=%u memcard=%s protocol=%u",
                RoleName(), m_username.c_str(), static_cast<unsigned>(m_port), static_cast<unsigned>(m_max_players),
                static_cast<unsigned>(m_delay.load()), m_memory_card_sync_enabled ? "on" : "off",
                static_cast<unsigned>(PROTOCOL_VERSION));
        }

        ~Session()
        {
            Stop();
        }

        bool IsConfigured() const { return m_role != Role::Disabled; }

        void StartSessionAsync()
        {
            if (!IsConfigured() || m_stop_requested.load(std::memory_order_acquire) ||
                m_failed.load(std::memory_order_acquire))
                return;

            bool expected = false;
            if (!m_connect_worker_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                return;

            if (!StartWinsock())
            {
                Fail("WSAStartup failed");
                return;
            }

            if (m_role == Role::Host)
            {
                m_connected.store(true, std::memory_order_release);
                m_connecting.store(m_max_players > 1, std::memory_order_release);
                if (m_max_players > 1)
                    m_connector = std::thread([this]() { AcceptLoop(); });
                else
                    Log("single-player room ready");
            }
            else
            {
                m_connecting.store(true, std::memory_order_release);
                m_connector = std::thread([this]() { ClientConnectAndReceive(); });
            }
        }

        StatusSnapshot GetStatusSnapshot() const
        {
            StatusSnapshot out;
            out.configured = IsConfigured();
            out.connecting = m_connecting.load(std::memory_order_acquire);
            out.connected = m_connected.load(std::memory_order_acquire);
            out.failed = m_failed.load(std::memory_order_acquire);
            out.delay = m_delay.load(std::memory_order_acquire);
            out.port = m_port;
            out.role = RoleName();
            out.username = m_username;

            std::lock_guard<std::mutex> lock(m_state_mutex);
            out.max_players = m_max_players;
            out.local_player_id = m_local_player_id;
            out.session_id = m_session_id;
            out.last_error = m_last_error;
            out.log_path = m_log_path;
            out.player_count = ConnectedCountLocked();
            out.room_full = (out.player_count == m_max_players);
            for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
            {
                out.players[i].id = i + 1;
                out.players[i].connected = m_players[i].connected;
                out.players[i].game_match = m_players[i].game_match;
                out.players[i].memcard_ready = m_players[i].memcard_ready;
                out.players[i].boot_ready = m_players[i].boot_ready;
                out.players[i].controller = m_players[i].controller;
                out.players[i].name = m_players[i].name;
            }

            out.game_selected = m_game_selected;
            out.local_game_match = m_local_game_match;
            out.all_games_match = AllGamesMatchLocked();
            out.game_title = m_game_title;
            out.game_serial = m_game_serial;
            out.game_crc = m_game_crc;
            out.local_game_path = m_local_game_path;

            out.memory_card_sync_enabled = m_memory_card_sync_enabled;
            out.memory_card_local_ready = m_memory_card_local_ready;
            out.memory_card_all_ready = AllMemcardsReadyLocked();
            out.memory_card_present = m_memory_card_present;
            out.memory_card_crc = m_memory_card_crc;
            out.memory_card_size = m_memory_card_size;
            out.memory_card_status = m_memory_card_status;

            out.start_requested = m_start_requested;
            out.prepare_boot = m_prepare_boot;
            out.local_boot_ready = m_local_boot_ready;
            out.all_boot_ready = AllBootReadyLocked();
            out.start_committed = m_start_committed;
            out.first_poll_released = m_first_poll_go;
            out.topology_mode = m_topology_mode.load(std::memory_order_acquire);
            out.runtime_reconfiguring = m_runtime_reconfiguring;
            out.input_epoch = m_input_epoch.load(std::memory_order_acquire);
            return out;
        }

        bool HostSelectGame(const std::string& path, const std::string& title,
            const std::string& serial, std::uint32_t crc)
        {
            if (m_role != Role::Host || path.empty() || serial.empty() || crc == 0)
                return false;

            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (m_prepare_boot || ConnectedCountLocked() != m_max_players)
                    return false;

                m_game_selected = true;
                m_local_game_match = true;
                m_game_title = title;
                m_game_serial = serial;
                m_game_crc = crc;
                m_local_game_path = path;
                m_last_error.clear();
                ResetStartStateLocked();
                for (std::uint32_t i = 0; i < m_max_players; i++)
                    m_players[i].game_match = (i == 0);
            }

            Log("host selected game: title=%s serial=%s crc=%08X", title.c_str(), serial.c_str(), crc);
            BroadcastRoster();
            if (!BroadcastGameManifest())
            {
                Fail("failed to broadcast game manifest");
                return false;
            }
            return true;
        }

        bool RequestSynchronizedBoot()
        {
            if (m_role != Role::Host || !m_game_selected)
                return false;

            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (ConnectedCountLocked() != m_max_players || m_prepare_boot)
                    return false;
                m_start_requested = true;
            }

            Log("start requested; waiting for game match and memory-card synchronization");
            MaybeAdvanceStart();
            return true;
        }

        bool ConsumeBootLaunchRequest(std::string* path)
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            if (!m_boot_launch_pending || m_boot_launch_consumed || m_local_game_path.empty())
                return false;
            m_boot_launch_consumed = true;
            if (path)
                *path = m_local_game_path;
            return true;
        }

        bool CanStartVM() const
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            return m_prepare_boot && m_boot_launch_consumed && !m_start_committed;
        }

        bool ShouldHoldBootBarrier() const
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            return m_prepare_boot && m_boot_launch_consumed;
        }

        void NotifyBootReady()
        {
            bool send_to_host = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!m_prepare_boot || m_local_boot_ready)
                    return;
                m_local_boot_ready = true;
                if (m_local_player_id >= 1 && m_local_player_id <= MAX_PLAYERS)
                    m_players[m_local_player_id - 1].boot_ready = true;
                send_to_host = (m_role == Role::Client);
            }

            Log("BOOT_READY: local VM initialized and waiting");
            if (send_to_host && !SendControlToHost(ControlType::BootReady, nullptr, 0))
            {
                Fail("failed to send BOOT_READY");
                return;
            }
            if (m_role == Role::Host)
            {
                BroadcastRoster();
                MaybeCommitStart();
            }
            m_boot_cv.notify_all();
        }

        bool WaitForStartCommit()
        {
            std::unique_lock<std::mutex> lock(m_state_mutex);
            const bool ready = m_boot_cv.wait_for(lock, std::chrono::seconds(BOOT_BARRIER_TIMEOUT_SECONDS), [this]() {
                return m_start_committed || m_failed.load(std::memory_order_acquire) ||
                    m_stop_requested.load(std::memory_order_acquire);
            });
            if (!ready || !m_start_committed)
            {
                lock.unlock();
                Fail("timed out waiting for synchronized START_COMMIT");
                return false;
            }
            Log("START_COMMIT observed; VM may enter Running state");
            return true;
        }

        bool ShouldForceDualShock2Slot(std::uint32_t slot) const
        {
            if (!IsConfigured())
                return false;

            // 1P/2P always use the two normal physical controller ports.
            if (m_max_players <= 2)
            {
                if (slot == 0) return m_max_players >= 1;
                if (slot == 1) return m_max_players >= 2;
                return false;
            }

            const std::uint32_t topology = m_topology_mode.load(std::memory_order_acquire);
            if (topology == 0)
            {
                if (slot == 0) return m_max_players >= 1;
                if (slot == 2) return m_max_players >= 2;
                if (slot == 3) return m_max_players >= 3;
                if (slot == 4) return m_max_players >= 4;
            }
            else
            {
                if (slot == 0) return m_max_players >= 1;
                if (slot == 1) return m_max_players >= 2;
                if (slot == 5) return m_max_players >= 3;
                if (slot == 6) return m_max_players >= 4;
            }
            return false;
        }

        bool ShouldDisconnectControllerSlot(std::uint32_t slot) const
        {
            return IsConfigured() && !ShouldForceDualShock2Slot(slot);
        }

        void ApplyDeterministicConfig()
        {
            if (!IsConfigured())
                return;

            const std::uint32_t topology = m_topology_mode.load(std::memory_order_acquire);
            EmuConfig.Pad.MultitapPort0_Enabled = (m_max_players >= 3 && topology == 0);
            EmuConfig.Pad.MultitapPort1_Enabled = (m_max_players >= 3 && topology == 1);

            if (m_max_players >= 3)
            {
                if (topology == 0)
                    Log("pad topology: P1=slot0 P2=slot2 P3=slot3%s; multitap=controller-port-1",
                        (m_max_players >= 4) ? " P4=slot4" : "");
                else
                    Log("pad topology: P1=slot0 P2=slot1 P3=slot5%s; multitap=controller-port-2",
                        (m_max_players >= 4) ? " P4=slot6" : "");
            }

            std::string shadow_name;
            bool memcard_ready = false;
            bool memcard_present = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                memcard_ready = m_memory_card_local_ready;
                memcard_present = m_memory_card_present;
                shadow_name = m_shadow_card_filename;
            }

            if (!m_memory_card_sync_enabled || !memcard_ready)
                return;

            for (std::uint32_t i = 0; i < 8; i++)
                EmuConfig.Mcd[i].Enabled = false;

            if (memcard_present && !shadow_name.empty())
            {
                EmuConfig.Mcd[0].Enabled = true;
                EmuConfig.Mcd[0].Type = MemoryCardType::File;
                EmuConfig.Mcd[0].Filename = shadow_name;
            }
        }

        std::uint8_t HandlePadResponse(std::uint8_t unified_slot,
            std::uint32_t command_index, std::uint8_t local_value)
        {
            if (m_role == Role::Disabled || command_index < 3 || command_index > 20)
                return local_value;

            const std::size_t input_index = static_cast<std::size_t>(command_index - 3);
            if (unified_slot == 0 && command_index == 3)
            {
                if (!BeginPadPoll())
                    return local_value;
            }

            if (!m_start_committed || !m_connected.load(std::memory_order_acquire))
                return local_value;

            // Every PC binds its physical controller to Pad1. Capture that raw
            // input before replacing Pad1 with the authoritative room bundle.
            if (unified_slot == 0)
                m_capture[input_index] = local_value;

            const int player_index = SlotToPlayerIndex(unified_slot);
            if (player_index < 0)
                return local_value;
            return m_output_bundle[static_cast<std::size_t>(player_index)][input_index];
        }

        void Stop()
        {
            if (m_stop_requested.exchange(true, std::memory_order_acq_rel))
                return;

            Log("session shutdown requested");
            m_connected.store(false, std::memory_order_release);
            m_connecting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            m_input_cv.notify_all();
            m_bundle_cv.notify_all();
            m_boot_cv.notify_all();
            m_first_poll_cv.notify_all();

            const SOCKET listener = m_listener.exchange(INVALID_SOCKET, std::memory_order_acq_rel);
            if (listener != INVALID_SOCKET)
                closesocket(listener);

            {
                std::lock_guard<std::mutex> lock(m_client_socket_mutex);
                if (m_client_socket != INVALID_SOCKET)
                {
                    shutdown(m_client_socket, SD_BOTH);
                    closesocket(m_client_socket);
                    m_client_socket = INVALID_SOCKET;
                }
            }

            {
                std::lock_guard<std::mutex> lock(m_peer_mutex);
                for (auto& peer : m_peers)
                {
                    if (peer && peer->socket != INVALID_SOCKET)
                    {
                        shutdown(peer->socket, SD_BOTH);
                        closesocket(peer->socket);
                        peer->socket = INVALID_SOCKET;
                    }
                }
            }

            if (m_connector.joinable() && m_connector.get_id() != std::this_thread::get_id())
                m_connector.join();

            std::array<std::thread*, 3> threads{};
            {
                std::lock_guard<std::mutex> lock(m_peer_mutex);
                for (std::size_t i = 0; i < m_peers.size(); i++)
                    threads[i] = m_peers[i] ? &m_peers[i]->receiver : nullptr;
            }
            for (std::thread* thread : threads)
            {
                if (thread && thread->joinable() && thread->get_id() != std::this_thread::get_id())
                    thread->join();
            }

            if (m_winsock_started)
            {
                WSACleanup();
                m_winsock_started = false;
            }

            std::string shadow_to_remove;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                shadow_to_remove = m_shadow_card_filename;
                m_shadow_card_filename.clear();
            }
            if (!shadow_to_remove.empty())
            {
                const std::string shadow_path = Path::Combine(EmuFolders::MemoryCards, shadow_to_remove);
                const bool removed = FileSystem::DeleteFilePath(shadow_path.c_str());
                Log("removed Netplay shadow card: %s%s", shadow_to_remove.c_str(),
                    removed ? "" : " (cleanup warning)");
            }

            Log("session stopped");
            CloseLog();
        }

        bool RequestRuntimeSettingsImpl(std::uint32_t local_controller,
            std::uint32_t delay, std::uint32_t topology)
        {
            return RequestRuntimeSettingsImpl(local_controller, delay, topology);
        }

    private:
        const char* RoleName() const
        {
            switch (m_role)
            {
                case Role::Host: return "host";
                case Role::Client: return "client";
                default: return "disabled";
            }
        }

        void ReadConfiguration()
        {
            const char* mode = std::getenv("PCSX2_NETPLAY_MODE");
            if (!mode)
                return;

            if (_stricmp(mode, "host") == 0)
                m_role = Role::Host;
            else if (_stricmp(mode, "client") == 0 || _stricmp(mode, "join") == 0)
                m_role = Role::Client;
            else
                return;

            if (const char* username = std::getenv("PCSX2_NETPLAY_USERNAME"))
                m_username = username;
            if (m_username.empty())
                m_username = (m_role == Role::Host) ? "房主" : "玩家";
            m_username = TruncateUtf8(m_username, 35);

            if (const char* host = std::getenv("PCSX2_NETPLAY_HOST"))
                m_host = host;
            if (const char* port = std::getenv("PCSX2_NETPLAY_PORT"))
            {
                const int value = std::atoi(port);
                if (value > 0 && value <= 65535)
                    m_port = static_cast<std::uint16_t>(value);
            }
            if (const char* delay = std::getenv("PCSX2_NETPLAY_DELAY"))
                m_delay.store(static_cast<std::uint32_t>(std::clamp(std::atoi(delay), 1, 100)), std::memory_order_release);
            if (const char* players = std::getenv("PCSX2_NETPLAY_PLAYERS"))
                m_max_players = static_cast<std::uint32_t>(std::clamp(std::atoi(players), 1, 4));
            if (const char* sync = std::getenv("PCSX2_NETPLAY_MEMCARD_SYNC"))
                m_memory_card_sync_enabled = (std::atoi(sync) != 0);
        }

        bool StartWinsock()
        {
            if (m_winsock_started)
                return true;
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                return false;
            m_winsock_started = true;
            return true;
        }

        void OpenLog()
        {
            std::error_code ec;
            const std::filesystem::path dir = std::filesystem::path("logs") / "netplay";
            std::filesystem::create_directories(dir, ec);
            std::time_t now = std::time(nullptr);
            std::tm local_tm{};
            localtime_s(&local_tm, &now);
            char stamp[32]{};
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local_tm);
            const std::string filename = std::string("netplay-") + stamp + "-" + RoleName() + ".log";
            const std::filesystem::path file_path = dir / filename;
            FILE* file = nullptr;
            if (fopen_s(&file, file_path.string().c_str(), "wb") == 0)
                m_log_file = file;
            const std::filesystem::path absolute_path = std::filesystem::absolute(file_path, ec);
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_log_path = ec ? file_path.string() : absolute_path.string();
        }

        void CloseLog()
        {
            std::lock_guard<std::mutex> lock(m_log_mutex);
            if (m_log_file)
            {
                std::fclose(m_log_file);
                m_log_file = nullptr;
            }
        }

        void Log(const char* format, ...) const
        {
            char message[2048]{};
            va_list args;
            va_start(args, format);
            std::vsnprintf(message, sizeof(message), format, args);
            va_end(args);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_session_started).count();
            std::lock_guard<std::mutex> lock(m_log_mutex);
            std::fprintf(stderr, "[ModernNetplay +%lldms] %s\n", static_cast<long long>(elapsed), message);
            if (m_log_file)
            {
                std::fprintf(m_log_file, "[+%lldms] %s\r\n", static_cast<long long>(elapsed), message);
                std::fflush(m_log_file);
            }
        }

        void SetLastError(const char* message)
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_last_error = message ? message : "unknown error";
        }

        std::uint32_t ConnectedCountLocked() const
        {
            std::uint32_t count = 0;
            for (std::uint32_t i = 0; i < m_max_players; i++)
                count += m_players[i].connected ? 1u : 0u;
            return count;
        }

        bool AllGamesMatchLocked() const
        {
            if (!m_game_selected || ConnectedCountLocked() != m_max_players)
                return false;
            for (std::uint32_t i = 0; i < m_max_players; i++)
            {
                if (!m_players[i].connected || !m_players[i].game_match)
                    return false;
            }
            return true;
        }

        bool AllMemcardsReadyLocked() const
        {
            if (!m_memory_card_sync_enabled)
                return true;
            if (ConnectedCountLocked() != m_max_players)
                return false;
            for (std::uint32_t i = 0; i < m_max_players; i++)
            {
                if (!m_players[i].connected || !m_players[i].memcard_ready)
                    return false;
            }
            return true;
        }

        bool AllBootReadyLocked() const
        {
            if (!m_prepare_boot || ConnectedCountLocked() != m_max_players)
                return false;
            for (std::uint32_t i = 0; i < m_max_players; i++)
            {
                if (!m_players[i].connected || !m_players[i].boot_ready)
                    return false;
            }
            return true;
        }

        bool AllFirstPollReadyLocked() const
        {
            if (!m_prepare_boot || ConnectedCountLocked() != m_max_players)
                return false;
            for (std::uint32_t i = 0; i < m_max_players; i++)
            {
                if (!m_players[i].connected || !m_players[i].first_poll_ready)
                    return false;
            }
            return true;
        }

        void ResetStartStateLocked()
        {
            m_start_requested = false;
            m_memcard_transfer_started = false;
            m_memory_card_local_ready = false;
            m_memory_card_present = false;
            m_memory_card_crc = 0;
            m_memory_card_size = 0;
            m_memory_card_status = m_memory_card_sync_enabled ? "等待记忆卡同步" : "记忆卡同步已关闭";
            m_shadow_card_filename.clear();
            m_prepare_boot = false;
            m_boot_launch_pending = false;
            m_boot_launch_consumed = false;
            m_local_boot_ready = false;
            m_start_committed = false;
            m_first_poll_go = false;
            m_frame = 0;
            m_have_capture = false;
            m_capture = NEUTRAL_FRAME;
            m_output_bundle.fill(NEUTRAL_FRAME);
            for (auto& player : m_players)
            {
                player.memcard_ready = false;
                player.boot_ready = false;
                player.first_poll_ready = false;
            }
            for (auto& map : m_player_inputs)
                map.clear();
            m_bundles.clear();
        }

        int SlotToPlayerIndex(std::uint32_t slot) const
        {
            // Never disturb the proven 1P/2P layout.
            if (m_max_players <= 2)
            {
                if (slot == 0) return 0;
                if (m_max_players >= 2 && slot == 1) return 1;
                return -1;
            }

            const std::uint32_t topology = m_topology_mode.load(std::memory_order_acquire);
            if (topology == 0)
            {
                if (slot == 0) return 0;
                if (m_max_players >= 2 && slot == 2) return 1;
                if (m_max_players >= 3 && slot == 3) return 2;
                if (m_max_players >= 4 && slot == 4) return 3;
            }
            else
            {
                if (slot == 0) return 0;
                if (m_max_players >= 2 && slot == 1) return 1;
                if (m_max_players >= 3 && slot == 5) return 2;
                if (m_max_players >= 4 && slot == 6) return 3;
            }
            return -1;
        }

        bool BuildHello(std::array<std::uint8_t, HELLO_SIZE>* packet, Role role,
            std::uint32_t max_players, std::uint32_t player_id, std::uint64_t session,
            bool memcard_sync, const std::string& name) const
        {
            packet->fill(0);
            WriteU32(packet->data() + 0, HELLO_MAGIC);
            WriteU32(packet->data() + 4, PROTOCOL_VERSION);
            WriteU32(packet->data() + 8, static_cast<std::uint32_t>(role));
            WriteU32(packet->data() + 12, max_players);
            WriteU32(packet->data() + 16, m_delay.load(std::memory_order_acquire));
            WriteU32(packet->data() + 20, player_id);
            WriteU64(packet->data() + 24, session);
            WriteU32(packet->data() + 32, memcard_sync ? 1u : 0u);
            std::snprintf(reinterpret_cast<char*>(packet->data() + 40), 40, "%s", name.c_str());
            return true;
        }

        static std::string ReadHelloName(const std::array<std::uint8_t, HELLO_SIZE>& packet)
        {
            const char* ptr = reinterpret_cast<const char*>(packet.data() + 40);
            std::size_t length = 0;
            while (length < 40 && ptr[length] != '\0')
                ++length;
            return std::string(ptr, length);
        }

        std::uint32_t FindFreePlayerIdLocked() const
        {
            for (std::uint32_t id = 2; id <= m_max_players; id++)
            {
                if (!m_players[id - 1].connected)
                    return id;
            }
            return 0;
        }

        bool HostHandshake(SOCKET socket, std::uint32_t* player_id, std::string* name)
        {
            std::array<std::uint8_t, HELLO_SIZE> incoming{};
            if (!ReceiveAll(socket, incoming.data(), incoming.size()))
                return false;
            if (ReadU32(incoming.data()) != HELLO_MAGIC || ReadU32(incoming.data() + 4) != PROTOCOL_VERSION ||
                ReadU32(incoming.data() + 8) != static_cast<std::uint32_t>(Role::Client))
                return false;

            std::uint32_t assigned = 0;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                assigned = FindFreePlayerIdLocked();
            }
            if (assigned == 0)
                return false;

            std::string client_name = ReadHelloName(incoming);
            if (client_name.empty())
                client_name = "玩家" + std::to_string(assigned);

            std::array<std::uint8_t, HELLO_SIZE> outgoing{};
            BuildHello(&outgoing, Role::Host, m_max_players, assigned, m_session_id,
                m_memory_card_sync_enabled, m_username);
            if (!SendAll(socket, outgoing.data(), outgoing.size()))
                return false;

            *player_id = assigned;
            *name = client_name;
            return true;
        }

        bool ClientHandshake(SOCKET socket)
        {
            std::array<std::uint8_t, HELLO_SIZE> outgoing{};
            BuildHello(&outgoing, Role::Client, 0, 0, 0, true, m_username);
            if (!SendAll(socket, outgoing.data(), outgoing.size()))
                return false;

            std::array<std::uint8_t, HELLO_SIZE> incoming{};
            if (!ReceiveAll(socket, incoming.data(), incoming.size()))
                return false;
            if (ReadU32(incoming.data()) != HELLO_MAGIC || ReadU32(incoming.data() + 4) != PROTOCOL_VERSION ||
                ReadU32(incoming.data() + 8) != static_cast<std::uint32_t>(Role::Host))
                return false;

            const std::uint32_t max_players = ReadU32(incoming.data() + 12);
            const std::uint32_t delay = ReadU32(incoming.data() + 16);
            const std::uint32_t assigned = ReadU32(incoming.data() + 20);
            if (max_players < 1 || max_players > MAX_PLAYERS || assigned < 2 || assigned > max_players)
                return false;

            const std::string host_name = ReadHelloName(incoming);
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_max_players = max_players;
                m_local_player_id = assigned;
                m_session_id = ReadU64(incoming.data() + 24);
                m_memory_card_sync_enabled = (ReadU32(incoming.data() + 32) != 0);
                m_players[0].connected = true;
                m_players[0].name = host_name.empty() ? "房主" : host_name;
                for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
                    m_players[i].controller = i + 1;
                m_players[assigned - 1].connected = true;
                m_players[assigned - 1].name = m_username;
                m_last_error.clear();
            }
            m_delay.store(std::clamp<std::uint32_t>(delay, 1, 100), std::memory_order_release);
            return true;
        }

        void AcceptLoop()
        {
            SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener == INVALID_SOCKET)
            {
                Fail("failed to create listening socket");
                return;
            }
            m_listener.store(listener, std::memory_order_release);
            BOOL reuse = TRUE;
            setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_ANY);
            address.sin_port = htons(m_port);
            if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
                listen(listener, SOMAXCONN) == SOCKET_ERROR)
            {
                Fail("failed to bind/listen Netplay room");
                return;
            }

            Log("room open: TCP port %u, capacity=%u", static_cast<unsigned>(m_port), static_cast<unsigned>(m_max_players));
            while (!m_stop_requested.load(std::memory_order_acquire))
            {
                {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    m_connecting.store(ConnectedCountLocked() < m_max_players, std::memory_order_release);
                }

                fd_set read_set;
                FD_ZERO(&read_set);
                FD_SET(listener, &read_set);
                timeval timeout{};
                timeout.tv_usec = 250000;
                const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
                if (ready == SOCKET_ERROR)
                    break;
                if (ready <= 0 || !FD_ISSET(listener, &read_set))
                    continue;

                sockaddr_storage peer_address{};
                int peer_address_length = sizeof(peer_address);
                SOCKET socket_value = accept(listener, reinterpret_cast<sockaddr*>(&peer_address), &peer_address_length);
                if (socket_value == INVALID_SOCKET)
                    continue;

                BOOL no_delay = TRUE;
                setsockopt(socket_value, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));

                std::uint32_t player_id = 0;
                std::string player_name;
                if (!HostHandshake(socket_value, &player_id, &player_name))
                {
                    closesocket(socket_value);
                    continue;
                }

                const std::size_t peer_index = static_cast<std::size_t>(player_id - 2);
                std::unique_ptr<Peer> old_peer;
                {
                    std::lock_guard<std::mutex> lock(m_peer_mutex);
                    old_peer = std::move(m_peers[peer_index]);
                }
                if (old_peer && old_peer->receiver.joinable())
                    old_peer->receiver.join();

                auto peer = std::make_unique<Peer>();
                peer->socket = socket_value;
                peer->player_id = player_id;
                peer->name = player_name;
                peer->connected.store(true, std::memory_order_release);
                Peer* raw_peer = peer.get();
                {
                    std::lock_guard<std::mutex> lock(m_peer_mutex);
                    m_peers[peer_index] = std::move(peer);
                }
                {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    m_players[player_id - 1] = PlayerState{};
                    m_players[player_id - 1].controller = player_id;
                    m_players[player_id - 1].connected = true;
                    m_players[player_id - 1].name = player_name;
                }
                raw_peer->receiver = std::thread([this, raw_peer]() { HostPeerReceiver(raw_peer); });
                Log("P%u joined: %s", static_cast<unsigned>(player_id), player_name.c_str());
                BroadcastRoster();

                bool selected = false;
                {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    selected = m_game_selected;
                }
                if (selected)
                    SendGameManifestToPeer(*raw_peer);
            }
        }

        void ClientConnectAndReceive()
        {
            unsigned attempt = 0;
            while (!m_stop_requested.load(std::memory_order_acquire))
            {
                ++attempt;
                addrinfo hints{};
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;
                hints.ai_protocol = IPPROTO_TCP;
                addrinfo* result = nullptr;
                const std::string port = std::to_string(m_port);
                if (getaddrinfo(m_host.c_str(), port.c_str(), &hints, &result) == 0)
                {
                    SOCKET connected = INVALID_SOCKET;
                    for (addrinfo* current = result; current; current = current->ai_next)
                    {
                        SOCKET candidate = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
                        if (candidate == INVALID_SOCKET)
                            continue;
                        if (ConnectWithTimeout(candidate, current->ai_addr,
                                static_cast<int>(current->ai_addrlen), 900))
                        {
                            connected = candidate;
                            break;
                        }
                        closesocket(candidate);
                    }
                    freeaddrinfo(result);
                    if (connected != INVALID_SOCKET)
                    {
                        BOOL no_delay = TRUE;
                        setsockopt(connected, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
                        if (ClientHandshake(connected))
                        {
                            {
                                std::lock_guard<std::mutex> lock(m_client_socket_mutex);
                                m_client_socket = connected;
                            }
                            m_connected.store(true, std::memory_order_release);
                            m_connecting.store(false, std::memory_order_release);
                            m_running.store(true, std::memory_order_release);
                            Log("joined room: assigned=P%u players=%u delay=%u session=%016llX",
                                static_cast<unsigned>(m_local_player_id), static_cast<unsigned>(m_max_players),
                                static_cast<unsigned>(m_delay.load()), static_cast<unsigned long long>(m_session_id));
                            ClientReceiverLoop(connected);

                            {
                                std::lock_guard<std::mutex> lock(m_client_socket_mutex);
                                if (m_client_socket == connected)
                                    m_client_socket = INVALID_SOCKET;
                            }
                            shutdown(connected, SD_BOTH);
                            closesocket(connected);
                            m_connected.store(false, std::memory_order_release);
                            m_running.store(false, std::memory_order_release);

                            if (m_stop_requested.load(std::memory_order_acquire) ||
                                m_failed.load(std::memory_order_acquire))
                                return;

                            {
                                std::lock_guard<std::mutex> lock(m_state_mutex);
                                m_local_player_id = 0;
                                m_session_id = 0;
                                m_players = {};
                                m_players[0].name = "房主";
                                m_game_selected = false;
                                m_local_game_match = false;
                                m_game_title.clear();
                                m_game_serial.clear();
                                m_game_crc = 0;
                                m_local_game_path.clear();
                                ResetStartStateLocked();
                            }
                            m_connecting.store(true, std::memory_order_release);
                            Log("connection returned to lobby; retrying host automatically");
                            std::this_thread::sleep_for(std::chrono::milliseconds(350));
                            continue;
                        }
                        closesocket(connected);
                    }
                }
                if (attempt == 1 || (attempt % 10) == 0)
                    Log("joining room: %s:%u attempt=%u", m_host.c_str(), static_cast<unsigned>(m_port), attempt);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }

        bool SendPacketLocked(SOCKET socket, std::mutex& mutex, const void* data, std::size_t size)
        {
            std::lock_guard<std::mutex> lock(mutex);
            return socket != INVALID_SOCKET && SendAll(socket, data, size);
        }

        bool SendControlRaw(SOCKET socket, std::mutex& mutex, ControlType type,
            const void* payload, std::uint32_t size)
        {
            if (size > MAX_CONTROL_PAYLOAD)
                return false;
            std::vector<std::uint8_t> packet(12u + size);
            WriteU32(packet.data(), CONTROL_MAGIC);
            WriteU32(packet.data() + 4, static_cast<std::uint32_t>(type));
            WriteU32(packet.data() + 8, size);
            if (size > 0)
                std::memcpy(packet.data() + 12, payload, size);
            return SendPacketLocked(socket, mutex, packet.data(), packet.size());
        }

        bool SendControlToHost(ControlType type, const void* payload, std::uint32_t size)
        {
            std::lock_guard<std::mutex> socket_lock(m_client_socket_mutex);
            return m_client_socket != INVALID_SOCKET && SendControlRaw(m_client_socket, m_client_send_mutex, type, payload, size);
        }

        bool SendControlToPeer(Peer& peer, ControlType type, const void* payload, std::uint32_t size)
        {
            return peer.connected.load(std::memory_order_acquire) &&
                SendControlRaw(peer.socket, peer.send_mutex, type, payload, size);
        }

        void BroadcastControl(ControlType type, const void* payload, std::uint32_t size)
        {
            std::lock_guard<std::mutex> lock(m_peer_mutex);
            for (auto& peer : m_peers)
            {
                if (peer && peer->connected.load(std::memory_order_acquire))
                {
                    if (!SendControlToPeer(*peer, type, payload, size))
                        Log("warning: failed control send to P%u", static_cast<unsigned>(peer->player_id));
                }
            }
        }

        void BroadcastRoster()
        {
            if (m_role != Role::Host)
                return;
            std::array<std::uint8_t, 4 + MAX_PLAYERS * 48> payload{};
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                WriteU32(payload.data(), m_max_players);
                for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
                {
                    std::uint8_t* row = payload.data() + 4 + i * 48;
                    WriteU32(row + 0, i + 1);
                    std::uint32_t flags = 0;
                    flags |= m_players[i].connected ? 1u : 0u;
                    flags |= m_players[i].game_match ? 2u : 0u;
                    flags |= m_players[i].memcard_ready ? 4u : 0u;
                    flags |= m_players[i].boot_ready ? 8u : 0u;
                    WriteU32(row + 4, flags);
                    WriteU32(row + 8, m_players[i].controller);
                    std::snprintf(reinterpret_cast<char*>(row + 12), 36, "%s", m_players[i].name.c_str());
                }
            }
            BroadcastControl(ControlType::Roster, payload.data(), static_cast<std::uint32_t>(payload.size()));
        }

        bool BuildGameManifest(std::array<std::uint8_t, 4 + 32 + 160>* payload) const
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            if (!m_game_selected)
                return false;
            payload->fill(0);
            WriteU32(payload->data(), m_game_crc);
            std::snprintf(reinterpret_cast<char*>(payload->data() + 4), 32, "%s", m_game_serial.c_str());
            std::snprintf(reinterpret_cast<char*>(payload->data() + 36), 160, "%s", m_game_title.c_str());
            return true;
        }

        bool BroadcastGameManifest()
        {
            if (m_max_players == 1)
                return true;
            std::array<std::uint8_t, 4 + 32 + 160> payload{};
            if (!BuildGameManifest(&payload))
                return false;
            BroadcastControl(ControlType::GameManifest, payload.data(), static_cast<std::uint32_t>(payload.size()));
            return true;
        }

        bool SendGameManifestToPeer(Peer& peer)
        {
            std::array<std::uint8_t, 4 + 32 + 160> payload{};
            if (!BuildGameManifest(&payload))
                return false;
            return SendControlToPeer(peer, ControlType::GameManifest, payload.data(), static_cast<std::uint32_t>(payload.size()));
        }

        void HandleRoster(const std::vector<std::uint8_t>& payload)
        {
            if (payload.size() != (4 + MAX_PLAYERS * 48))
                return;
            const std::uint32_t max_players = ReadU32(payload.data());
            if (max_players < 1 || max_players > MAX_PLAYERS)
                return;
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_max_players = max_players;
            for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
            {
                const std::uint8_t* row = payload.data() + 4 + i * 48;
                const std::uint32_t id = ReadU32(row);
                if (id != i + 1)
                    continue;
                const std::uint32_t flags = ReadU32(row + 4);
                m_players[i].connected = (flags & 1u) != 0;
                m_players[i].game_match = (flags & 2u) != 0;
                m_players[i].memcard_ready = (flags & 4u) != 0;
                m_players[i].boot_ready = (flags & 8u) != 0;
                m_players[i].controller = ReadU32(row + 8);
                const char* name = reinterpret_cast<const char*>(row + 12);
                std::size_t len = 0;
                while (len < 36 && name[len] != '\0')
                    ++len;
                m_players[i].name.assign(name, len);
            }
        }

        void HandleGameManifest(const std::vector<std::uint8_t>& payload)
        {
            if (m_role != Role::Client || payload.size() != (4 + 32 + 160))
                return;
            const std::uint32_t crc = ReadU32(payload.data());
            const char* serial_ptr = reinterpret_cast<const char*>(payload.data() + 4);
            const char* title_ptr = reinterpret_cast<const char*>(payload.data() + 36);
            std::size_t serial_len = 0;
            while (serial_len < 32 && serial_ptr[serial_len] != '\0')
                ++serial_len;
            std::size_t title_len = 0;
            while (title_len < 160 && title_ptr[title_len] != '\0')
                ++title_len;
            const std::string serial(serial_ptr, serial_len);
            const std::string title(title_ptr, title_len);

            std::string local_path;
            {
                auto lock = GameList::GetLock();
                const GameList::Entry* entry = GameList::GetEntryBySerialAndCRC(serial, crc);
                if (entry)
                    local_path = entry->path;
            }
            const bool matched = !local_path.empty();
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_game_selected = true;
                m_game_title = title;
                m_game_serial = serial;
                m_game_crc = crc;
                m_local_game_path = local_path;
                m_local_game_match = matched;
                m_last_error.clear();
                ResetStartStateLocked();
                if (m_local_player_id >= 1 && m_local_player_id <= MAX_PLAYERS)
                    m_players[m_local_player_id - 1].game_match = matched;
                m_players[0].game_match = true;
            }
            std::array<std::uint8_t, 8> reply{};
            WriteU32(reply.data(), m_local_player_id);
            WriteU32(reply.data() + 4, matched ? 1u : 0u);
            SendControlToHost(ControlType::GameMatch, reply.data(), static_cast<std::uint32_t>(reply.size()));
            Log("GAME_MANIFEST: title=%s serial=%s crc=%08X local_match=%s",
                title.c_str(), serial.c_str(), crc, matched ? "yes" : "no");
        }

        bool ExportConfiguredMemoryCard(std::vector<std::uint8_t>* data, bool* present, std::string* error)
        {
            data->clear();
            *present = false;

            // The Netplay lobby runs before VM settings/type auto-detection.
            // Read the actual base Slot 1 selection and inspect the filesystem
            // instead of trusting the pre-VM EmuConfig.Mcd[0].Type value.
            const bool enabled = Host::GetBaseBoolSettingValue("MemoryCards", "Slot1_Enable", true);
            const std::string filename = Host::GetBaseStringSettingValue(
                "MemoryCards", "Slot1_Filename", FileMcd_GetDefaultName(0).c_str());

            if (!enabled || filename.empty())
            {
                Log("memory card source: slot1 disabled/empty; synchronizing no-card state");
                return true;
            }

            std::optional<AvailableMcdInfo> info = FileMcd_GetCardInfo(filename);
            if (!info.has_value())
            {
                // PCSX2 normally auto-creates a missing enabled file card when a
                // VM opens. Netplay must export before boot, so create the same
                // default 8 MB file card now and then export it.
                Log("memory card source '%s' missing; creating default 8 MB card before sync", filename.c_str());
                if (!FileMcd_CreateNewCard(filename, MemoryCardType::File, MemoryCardFileType::PS2_8MB))
                {
                    *error = "无法创建房主当前配置的 1 号记忆卡";
                    return false;
                }
                info = FileMcd_GetCardInfo(filename);
            }

            if (!info.has_value())
            {
                *error = "无法解析房主当前配置的 1 号记忆卡";
                return false;
            }

            Log("memory card source resolved: name=%s type=%s path=%s size=%u utf8_path=yes",
                filename.c_str(), info->type == MemoryCardType::Folder ? "folder" : "file",
                info->path.c_str(), static_cast<unsigned>(info->size));

            if (info->type == MemoryCardType::File)
            {
                std::optional<std::vector<u8>> file_data = FileSystem::ReadBinaryFile(info->path.c_str());
                if (!file_data.has_value())
                {
                    *error = "无法读取房主记忆卡文件：" + info->path;
                    return false;
                }
                if (file_data->empty() || file_data->size() > (80u * 1024u * 1024u))
                {
                    *error = "房主记忆卡大小异常";
                    return false;
                }

                data->assign(file_data->begin(), file_data->end());
                Log("memory card file read via PCSX2 filesystem: bytes=%u",
                    static_cast<unsigned>(data->size()));
                *present = true;
                return true;
            }

            if (info->type == MemoryCardType::Folder)
            {
                Pcsx2Config::McdOptions config{};
                config.Enabled = true;
                config.Type = MemoryCardType::Folder;
                config.Filename = filename;

                FolderMemoryCard card;
                card.Open(info->path, config, 0, false, "");
                if (card.IsPresent() <= 0)
                {
                    card.Close(false);
                    *error = "无法打开房主文件夹记忆卡：" + info->path;
                    return false;
                }

                const std::size_t capacity =
                    static_cast<std::size_t>(card.GetSizeInClusters()) * FolderMemoryCard::ClusterSizeRaw;
                if (capacity == 0 || capacity > (80u * 1024u * 1024u))
                {
                    card.Close(false);
                    *error = "房主文件夹记忆卡大小异常";
                    return false;
                }

                data->resize(capacity);
                std::size_t address = 0;
                while (address < capacity)
                {
                    const int size = static_cast<int>(
                        std::min<std::size_t>(FolderMemoryCard::PageSizeRaw, capacity - address));
                    if (card.Read(data->data() + address, static_cast<u32>(address), size) < 0)
                    {
                        card.Close(false);
                        *error = "导出房主文件夹记忆卡失败";
                        return false;
                    }
                    address += static_cast<std::size_t>(size);
                }
                card.Close(false);
                *present = true;
                return true;
            }

            *error = "当前记忆卡类型暂不支持联机同步";
            return false;
        }

        bool WriteShadowCard(const std::vector<std::uint8_t>& data, bool present, std::string* out_filename)
        {
            if (!present)
            {
                out_filename->clear();
                return true;
            }

            if (!FileSystem::DirectoryExists(EmuFolders::MemoryCards.c_str()) &&
                !FileSystem::CreateDirectoryPath(EmuFolders::MemoryCards.c_str(), true))
            {
                Log("failed to create memory-card directory: %s", EmuFolders::MemoryCards.c_str());
                return false;
            }

            char name[96]{};
            std::snprintf(name, sizeof(name), "NetplayShadow-%016llX.ps2",
                static_cast<unsigned long long>(m_session_id));
            const std::string path = Path::Combine(EmuFolders::MemoryCards, name);
            if (!FileSystem::WriteBinaryFile(path.c_str(), data.data(), data.size()))
            {
                Log("failed to write Netplay shadow card: %s", path.c_str());
                return false;
            }

            Log("Netplay shadow card written via PCSX2 filesystem: %s bytes=%u",
                path.c_str(), static_cast<unsigned>(data.size()));
            *out_filename = name;
            return true;
        }

        void CancelPendingStart(const std::string& message)
        {
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                ResetStartStateLocked();
                m_last_error = message;
                m_memory_card_status = message;
            }
            Log("START_CANCELLED: %s", message.c_str());
            BroadcastRoster();
        }

        void StartHostMemoryCardSync()
        {
            std::vector<std::uint8_t> data;
            bool present = false;
            std::string error;
            if (!ExportConfiguredMemoryCard(&data, &present, &error))
            {
                CancelPendingStart(error);
                return;
            }

            const std::uint64_t hash = present ? HashBytes(data) : 0;
            std::string shadow;
            if (!WriteShadowCard(data, present, &shadow))
            {
                CancelPendingStart("无法创建房主联机临时记忆卡");
                return;
            }

            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_memory_card_present = present;
                m_memory_card_crc = hash;
                m_memory_card_size = static_cast<std::uint32_t>(data.size());
                m_shadow_card_filename = shadow;
                m_memory_card_local_ready = true;
                m_players[0].memcard_ready = true;
                m_memory_card_status = present ? "房主临时记忆卡已创建，正在同步给其他玩家" : "房主未插入记忆卡，已同步为无卡状态";
            }

            std::array<std::uint8_t, 16> begin{};
            WriteU32(begin.data(), present ? 1u : 0u);
            WriteU32(begin.data() + 4, static_cast<std::uint32_t>(data.size()));
            WriteU64(begin.data() + 8, hash);
            BroadcastControl(ControlType::MemcardBegin, begin.data(), static_cast<std::uint32_t>(begin.size()));

            for (std::uint32_t offset = 0; offset < data.size(); offset += MEMCARD_CHUNK)
            {
                const std::uint32_t chunk_size = std::min<std::uint32_t>(MEMCARD_CHUNK,
                    static_cast<std::uint32_t>(data.size()) - offset);
                std::vector<std::uint8_t> chunk(4u + chunk_size);
                WriteU32(chunk.data(), offset);
                std::memcpy(chunk.data() + 4, data.data() + offset, chunk_size);
                BroadcastControl(ControlType::MemcardChunk, chunk.data(), static_cast<std::uint32_t>(chunk.size()));
            }
            BroadcastControl(ControlType::MemcardEnd, nullptr, 0);
            BroadcastRoster();
            Log("memory card shadow prepared: present=%s size=%u hash=%016llX",
                present ? "yes" : "no", static_cast<unsigned>(data.size()), static_cast<unsigned long long>(hash));
            MaybeAdvanceStart();
        }

        void HandleClientMemcardBegin(const std::vector<std::uint8_t>& payload)
        {
            if (payload.size() != 16)
                return;
            const bool present = ReadU32(payload.data()) != 0;
            const std::uint32_t size = ReadU32(payload.data() + 4);
            const std::uint64_t hash = ReadU64(payload.data() + 8);
            if (size > 80u * 1024u * 1024u)
            {
                Fail("memory card transfer is too large");
                return;
            }
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_memory_card_present = present;
            m_memory_card_crc = hash;
            m_memory_card_size = size;
            m_memcard_receive.assign(size, 0);
            m_memcard_received_bytes = 0;
            m_memory_card_local_ready = false;
            m_memory_card_status = present ? "正在接收房主记忆卡临时副本" : "房主未插入记忆卡";
        }

        void HandleClientMemcardChunk(const std::vector<std::uint8_t>& payload)
        {
            if (payload.size() < 4)
                return;
            const std::uint32_t offset = ReadU32(payload.data());
            const std::uint32_t size = static_cast<std::uint32_t>(payload.size() - 4);
            std::lock_guard<std::mutex> lock(m_state_mutex);
            if (offset > m_memcard_receive.size() || size > (m_memcard_receive.size() - offset))
            {
                m_last_error = "记忆卡数据块越界";
                return;
            }
            if (size > 0)
                std::memcpy(m_memcard_receive.data() + offset, payload.data() + 4, size);
            m_memcard_received_bytes += size;
        }

        void HandleClientMemcardEnd()
        {
            std::vector<std::uint8_t> data;
            bool present = false;
            std::uint64_t expected = 0;
            std::uint64_t received = 0;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                data = m_memcard_receive;
                present = m_memory_card_present;
                expected = m_memory_card_crc;
                received = m_memcard_received_bytes;
            }
            const std::uint64_t actual = present ? HashBytes(data) : 0;
            bool ok = (received == data.size()) && (actual == expected);
            std::string shadow;
            if (ok)
                ok = WriteShadowCard(data, present, &shadow);

            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_memory_card_local_ready = ok;
                m_shadow_card_filename = ok ? shadow : std::string();
                if (m_local_player_id >= 1 && m_local_player_id <= MAX_PLAYERS)
                    m_players[m_local_player_id - 1].memcard_ready = ok;
                m_memory_card_status = ok ? (present ? "记忆卡同步完成 ✓（使用联机临时副本）" : "无记忆卡状态同步完成 ✓") : "记忆卡同步失败";
                m_memcard_receive.clear();
            }

            std::array<std::uint8_t, 12> reply{};
            WriteU32(reply.data(), ok ? 1u : 0u);
            WriteU64(reply.data() + 4, actual);
            SendControlToHost(ControlType::MemcardReady, reply.data(), static_cast<std::uint32_t>(reply.size()));
            Log("memory card receive complete: ok=%s size=%u hash=%016llX", ok ? "yes" : "no",
                static_cast<unsigned>(data.size()), static_cast<unsigned long long>(actual));
            if (!ok)
            {
                SetLastError("记忆卡同步失败，可在房间中重新选择游戏重试");
                Log("memory card synchronization failed; lobby remains open for retry");
            }
        }

        void MaybeAdvanceStart()
        {
            if (m_role != Role::Host)
                return;

            bool start_memcard = false;
            bool begin_boot = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!m_start_requested || m_prepare_boot || ConnectedCountLocked() != m_max_players || !AllGamesMatchLocked())
                    return;

                if (m_memory_card_sync_enabled)
                {
                    if (!m_memcard_transfer_started)
                    {
                        m_memcard_transfer_started = true;
                        start_memcard = true;
                    }
                    else if (AllMemcardsReadyLocked())
                    {
                        begin_boot = true;
                    }
                }
                else
                {
                    m_memory_card_local_ready = true;
                    for (std::uint32_t i = 0; i < m_max_players; i++)
                        m_players[i].memcard_ready = true;
                    m_memory_card_status = "记忆卡同步已关闭";
                    begin_boot = true;
                }
            }

            if (start_memcard)
            {
                StartHostMemoryCardSync();
                return;
            }
            if (!begin_boot)
                return;

            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (m_prepare_boot)
                    return;
                m_prepare_boot = true;
                m_boot_launch_pending = true;
                m_boot_launch_consumed = false;
                m_local_boot_ready = false;
                for (std::uint32_t i = 0; i < m_max_players; i++)
                {
                    m_players[i].boot_ready = false;
                    m_players[i].first_poll_ready = false;
                }
            }
            Log("all players matched and memory cards ready; PREPARE_BOOT");
            BroadcastControl(ControlType::PrepareBoot, nullptr, 0);
            BroadcastRoster();
        }

        void HandleClientPrepareBoot()
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            if (!m_game_selected || !m_local_game_match || m_local_game_path.empty() ||
                (m_memory_card_sync_enabled && !m_memory_card_local_ready))
            {
                m_last_error = "收到启动命令，但本机游戏或记忆卡尚未准备完成";
                return;
            }
            m_prepare_boot = true;
            m_boot_launch_pending = true;
            m_boot_launch_consumed = false;
            m_local_boot_ready = false;
            m_start_requested = true;
            m_start_committed = false;
            m_first_poll_go = false;
            for (auto& player : m_players)
            {
                player.boot_ready = false;
                player.first_poll_ready = false;
            }
            Log("PREPARE_BOOT received; local matching game queued");
        }

        void MaybeCommitStart()
        {
            if (m_role != Role::Host)
                return;
            bool commit = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (AllBootReadyLocked() && !m_start_committed)
                {
                    m_start_committed = true;
                    commit = true;
                }
            }
            if (!commit)
                return;
            Log("all VMs BOOT_READY; START_COMMIT");
            BroadcastControl(ControlType::StartCommit, nullptr, 0);
            m_boot_cv.notify_all();
        }

        void MaybeReleaseFirstPoll()
        {
            if (m_role != Role::Host)
                return;
            bool release = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (m_start_committed && AllFirstPollReadyLocked() && !m_first_poll_go)
                {
                    m_first_poll_go = true;
                    release = true;
                }
            }
            if (!release)
                return;
            Log("all players reached first DS2 poll; FIRST_POLL_GO");
            BroadcastControl(ControlType::FirstPollGo, nullptr, 0);
            m_first_poll_cv.notify_all();
        }

        bool WaitForFirstPollBarrier()
        {
            bool send_ready = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!m_prepare_boot || m_first_poll_go)
                    return true;
                PlayerState& local = m_players[m_local_player_id - 1];
                if (!local.first_poll_ready)
                {
                    local.first_poll_ready = true;
                    send_ready = (m_role == Role::Client);
                }
            }

            if (send_ready && !SendControlToHost(ControlType::FirstPollReady, nullptr, 0))
            {
                Fail("failed to send FIRST_POLL_READY");
                return false;
            }
            if (m_role == Role::Host)
                MaybeReleaseFirstPoll();

            std::unique_lock<std::mutex> lock(m_state_mutex);
            const bool released = m_first_poll_cv.wait_for(lock, std::chrono::seconds(BOOT_BARRIER_TIMEOUT_SECONDS), [this]() {
                return m_first_poll_go || m_failed.load(std::memory_order_acquire) ||
                    m_stop_requested.load(std::memory_order_acquire);
            });
            if (!released || !m_first_poll_go)
            {
                lock.unlock();
                Fail("timed out waiting for FIRST_POLL_GO");
                return false;
            }
            return true;
        }

        void LogLocalInputTransition(std::uint32_t frame, const InputFrame& input)
        {
            bool changed = (input[0] != m_last_logged_local_input[0] || input[1] != m_last_logged_local_input[1]);
            for (std::size_t i = 6; i < INPUT_FRAME_BYTES && !changed; i++)
                changed = (input[i] != m_last_logged_local_input[i]);
            if (!changed)
                return;

            std::uint32_t local_controller = m_local_player_id;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (m_local_player_id >= 1 && m_local_player_id <= MAX_PLAYERS)
                    local_controller = m_players[m_local_player_id - 1].controller;
            }
            Log("INPUT P%u poll=%u digital=%02X %02X pressure[dpad R,L,U,D=%u,%u,%u,%u face T,O,X,S=%u,%u,%u,%u shoulders=%u,%u,%u,%u]",
                static_cast<unsigned>(local_controller), static_cast<unsigned>(frame),
                static_cast<unsigned>(input[0]), static_cast<unsigned>(input[1]),
                static_cast<unsigned>(input[6]), static_cast<unsigned>(input[7]),
                static_cast<unsigned>(input[8]), static_cast<unsigned>(input[9]),
                static_cast<unsigned>(input[10]), static_cast<unsigned>(input[11]),
                static_cast<unsigned>(input[12]), static_cast<unsigned>(input[13]),
                static_cast<unsigned>(input[14]), static_cast<unsigned>(input[15]),
                static_cast<unsigned>(input[16]), static_cast<unsigned>(input[17]));
            m_last_logged_local_input = input;
        }

        bool ValidateControllerMap(const std::array<std::uint32_t, MAX_PLAYERS>& controllers) const
        {
            std::array<bool, MAX_PLAYERS> seen{};
            for (std::uint32_t i = 0; i < m_max_players; i++)
            {
                const std::uint32_t value = controllers[i];
                if (value < 1 || value > m_max_players || seen[value - 1])
                    return false;
                seen[value - 1] = true;
            }
            return true;
        }

        std::array<std::uint32_t, MAX_PLAYERS> CurrentControllerMap() const
        {
            std::array<std::uint32_t, MAX_PLAYERS> out{1, 2, 3, 4};
            std::lock_guard<std::mutex> lock(m_state_mutex);
            for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
                out[i] = m_players[i].controller ? m_players[i].controller : (i + 1);
            return out;
        }

        std::array<std::uint8_t, RUNTIME_CONFIG_SIZE> BuildRuntimeConfig(
            std::uint32_t change_id, std::uint32_t delay, std::uint32_t topology,
            const std::array<std::uint32_t, MAX_PLAYERS>& controllers) const
        {
            std::array<std::uint8_t, RUNTIME_CONFIG_SIZE> payload{};
            WriteU32(payload.data() + 0, change_id);
            WriteU32(payload.data() + 4, delay);
            WriteU32(payload.data() + 8, topology);
            for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
                WriteU32(payload.data() + 12 + i * 4, controllers[i]);
            return payload;
        }

        bool ParseRuntimeConfig(const std::vector<std::uint8_t>& payload,
            std::uint32_t* change_id, std::uint32_t* delay, std::uint32_t* topology,
            std::array<std::uint32_t, MAX_PLAYERS>* controllers) const
        {
            if (payload.size() != RUNTIME_CONFIG_SIZE)
                return false;
            *change_id = ReadU32(payload.data() + 0);
            *delay = ReadU32(payload.data() + 4);
            *topology = ReadU32(payload.data() + 8);
            for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
                (*controllers)[i] = ReadU32(payload.data() + 12 + i * 4);
            return *change_id != 0 && *delay >= 1 && *delay <= 100 &&
                *topology <= 1 && ValidateControllerMap(*controllers);
        }

        void PauseForRuntimeReconfigure()
        {
            if (!VMManager::HasValidVM())
                return;
            Host::RunOnCPUThread([this]() {
                if (!VMManager::HasValidVM())
                    return;
                const VMState state = VMManager::GetState();
                {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    m_resume_after_reconfig = (state == VMState::Running);
                }
                if (state == VMState::Running)
                    VMManager::SetState(VMState::Paused);
            }, true);
        }

        void ReloadRuntimePads()
        {
            if (!VMManager::HasValidVM())
                return;
            Host::RunOnCPUThread([this]() {
                if (!VMManager::HasValidVM())
                    return;
                const std::uint32_t topology = m_topology_mode.load(std::memory_order_acquire);
                EmuConfig.Pad.MultitapPort0_Enabled = (m_max_players >= 3 && topology == 0);
                EmuConfig.Pad.MultitapPort1_Enabled = (m_max_players >= 3 && topology == 1);
                auto lock = Host::GetSettingsLock();
                Pad::LoadConfig(*Host::GetSettingsInterface());
            }, true);
        }

        void ResetRuntimeInputEpoch(std::uint32_t change_id)
        {
            m_input_epoch.store(change_id, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(m_input_mutex);
                for (auto& map : m_player_inputs)
                    map.clear();
            }
            {
                std::lock_guard<std::mutex> lock(m_bundle_mutex);
                m_bundles.clear();
            }
            m_frame = 0;
            m_have_capture = false;
            m_capture = NEUTRAL_FRAME;
            m_last_logged_local_input = NEUTRAL_FRAME;
            m_output_bundle.fill(NEUTRAL_FRAME);
            m_input_cv.notify_all();
            m_bundle_cv.notify_all();
        }

        bool ApplyRuntimeConfigLocal(std::uint32_t change_id, std::uint32_t delay,
            std::uint32_t topology, const std::array<std::uint32_t, MAX_PLAYERS>& controllers)
        {
            if (delay < 1 || delay > 100 || topology > 1 || !ValidateControllerMap(controllers))
                return false;
            PauseForRuntimeReconfigure();
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_runtime_reconfiguring = true;
                m_runtime_change_id = change_id;
                for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
                    m_players[i].controller = controllers[i];
                m_first_poll_go = false;
                for (auto& player : m_players)
                    player.first_poll_ready = false;
            }
            m_delay.store(delay, std::memory_order_release);
            m_topology_mode.store(topology, std::memory_order_release);
            ResetRuntimeInputEpoch(change_id);
            ReloadRuntimePads();
            Log("runtime config locally applied: change=%u delay=%u topology=%u controllers=%u,%u,%u,%u",
                static_cast<unsigned>(change_id), static_cast<unsigned>(delay), static_cast<unsigned>(topology),
                static_cast<unsigned>(controllers[0]), static_cast<unsigned>(controllers[1]),
                static_cast<unsigned>(controllers[2]), static_cast<unsigned>(controllers[3]));
            return true;
        }

        void FinishRuntimeConfig(std::uint32_t change_id)
        {
            bool resume = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!m_runtime_reconfiguring || m_runtime_change_id != change_id)
                    return;
                m_runtime_reconfiguring = false;
                resume = m_resume_after_reconfig;
                m_resume_after_reconfig = false;
            }
            if (resume && VMManager::HasValidVM())
            {
                Host::RunOnCPUThread([]() {
                    if (VMManager::HasValidVM() && VMManager::GetState() == VMState::Paused)
                        VMManager::SetState(VMState::Running);
                });
            }
            Host::AddKeyedOSDMessage("ModernNetplayRuntimeConfig",
                "联机实时设置已同步应用。", 5.0f);
            Log("runtime config committed: change=%u", static_cast<unsigned>(change_id));
        }

        void MaybeCommitRuntimeConfig()
        {
            if (m_role != Role::Host)
                return;
            std::uint32_t change_id = 0;
            bool ready = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!m_runtime_reconfiguring)
                    return;
                ready = true;
                for (std::uint32_t i = 0; i < m_max_players; i++)
                {
                    if (m_players[i].connected && !m_runtime_acks[i])
                    {
                        ready = false;
                        break;
                    }
                }
                change_id = m_runtime_change_id;
            }
            if (!ready)
                return;
            std::array<std::uint8_t, 4> payload{};
            WriteU32(payload.data(), change_id);
            BroadcastControl(ControlType::RuntimeCommit, payload.data(), static_cast<std::uint32_t>(payload.size()));
            FinishRuntimeConfig(change_id);
            BroadcastRoster();
        }

        bool BeginHostRuntimeConfig(std::uint32_t delay, std::uint32_t topology,
            const std::array<std::uint32_t, MAX_PLAYERS>& controllers)
        {
            if (m_role != Role::Host || delay < 1 || delay > 100 || topology > 1 ||
                !ValidateControllerMap(controllers))
                return false;
            std::uint32_t change_id = 0;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (m_runtime_reconfiguring)
                    return false;
                change_id = ++m_runtime_change_counter;
                if (change_id == 0)
                    change_id = ++m_runtime_change_counter;
                m_runtime_reconfiguring = true;
                m_runtime_change_id = change_id;
                m_runtime_acks.fill(false);
            }
            const auto payload = BuildRuntimeConfig(change_id, delay, topology, controllers);
            BroadcastControl(ControlType::RuntimeApply, payload.data(), static_cast<std::uint32_t>(payload.size()));
            if (!ApplyRuntimeConfigLocal(change_id, delay, topology, controllers))
            {
                Fail("failed to apply host runtime Netplay configuration");
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (m_runtime_reconfiguring && m_runtime_change_id == change_id)
                    m_runtime_acks[0] = true;
            }
            BroadcastRoster();
            MaybeCommitRuntimeConfig();
            return true;
        }

        bool RequestRuntimeSettings(std::uint32_t local_controller,
            std::uint32_t delay, std::uint32_t topology)
        {
            if (local_controller < 1 || local_controller > m_max_players)
                return false;
            if (m_role == Role::Client)
            {
                std::array<std::uint8_t, 4> payload{};
                WriteU32(payload.data(), local_controller);
                return SendControlToHost(ControlType::RuntimeRequest,
                    payload.data(), static_cast<std::uint32_t>(payload.size()));
            }
            if (m_role != Role::Host)
                return false;

            delay = std::clamp<std::uint32_t>(delay, 1, 100);
            topology = std::min<std::uint32_t>(topology, 1);
            auto controllers = CurrentControllerMap();
            const std::uint32_t current = controllers[0];
            if (current != local_controller)
            {
                for (std::uint32_t i = 0; i < m_max_players; i++)
                {
                    if (controllers[i] == local_controller)
                    {
                        controllers[i] = current;
                        break;
                    }
                }
                controllers[0] = local_controller;
            }
            return BeginHostRuntimeConfig(delay, topology, controllers);
        }

        void HandleRuntimeRequest(std::uint32_t requester_id, std::uint32_t desired_controller)
        {
            if (m_role != Role::Host || requester_id < 1 || requester_id > m_max_players ||
                desired_controller < 1 || desired_controller > m_max_players)
                return;
            auto controllers = CurrentControllerMap();
            const std::uint32_t requester = requester_id - 1;
            const std::uint32_t current = controllers[requester];
            if (current != desired_controller)
            {
                for (std::uint32_t i = 0; i < m_max_players; i++)
                {
                    if (controllers[i] == desired_controller)
                    {
                        controllers[i] = current;
                        break;
                    }
                }
                controllers[requester] = desired_controller;
            }
            BeginHostRuntimeConfig(m_delay.load(std::memory_order_acquire),
                m_topology_mode.load(std::memory_order_acquire), controllers);
        }

        void BroadcastSessionAbort(const std::string& reason)
        {
            if (m_role != Role::Host)
                return;

            const std::size_t size = std::min<std::size_t>(reason.size(), MAX_CONTROL_PAYLOAD);
            BroadcastControl(ControlType::SessionAbort,
                size > 0 ? reinterpret_cast<const std::uint8_t*>(reason.data()) : nullptr,
                size);
            Log("SESSION_ABORT broadcast: %s", reason.c_str());
        }

        void NotifyFatalSessionFailure(const std::string& reason)
        {
            if (!VMManager::HasValidVM())
                return;

            const std::string message =
                "检测到联机连接或同步异常，当前游戏已暂停。\n\n原因：" + reason +
                "\n\n请不要继续单独推进游戏。建议所有玩家回到联机大厅重新连接；如问题重复出现，请导出诊断包。";

            Host::RunOnCPUThread([message]() {
                if (VMManager::HasValidVM())
                {
                    VMManager::SetState(VMState::Paused);
                    Host::AddKeyedOSDMessage("ModernNetplayDisconnected",
                        "联机已中断，游戏已暂停。", Host::OSD_CRITICAL_ERROR_DURATION);
                }
            });
            Host::ReportErrorAsync("PCSX2 联机已中断", message);
        }

        bool SendInputToHost(std::uint32_t frame, const InputFrame& input)
        {
            std::array<std::uint8_t, INPUT_PACKET_SIZE> packet{};
            WriteU32(packet.data(), INPUT_MAGIC);
            WriteU32(packet.data() + 4, frame);
            WriteU32(packet.data() + 8, m_local_player_id);
            WriteU32(packet.data() + 12, m_input_epoch.load(std::memory_order_acquire));
            std::memcpy(packet.data() + 16, input.data(), input.size());
            std::lock_guard<std::mutex> socket_lock(m_client_socket_mutex);
            return m_client_socket != INVALID_SOCKET &&
                SendPacketLocked(m_client_socket, m_client_send_mutex, packet.data(), packet.size());
        }

        void BroadcastBundle(std::uint32_t frame, const InputBundle& bundle)
        {
            std::array<std::uint8_t, BUNDLE_PACKET_SIZE> packet{};
            WriteU32(packet.data(), BUNDLE_MAGIC);
            WriteU32(packet.data() + 4, frame);
            WriteU32(packet.data() + 8, m_max_players);
            WriteU32(packet.data() + 12, m_input_epoch.load(std::memory_order_acquire));
            for (std::size_t i = 0; i < MAX_PLAYERS; i++)
                std::memcpy(packet.data() + 16 + i * INPUT_FRAME_BYTES, bundle[i].data(), INPUT_FRAME_BYTES);

            std::lock_guard<std::mutex> lock(m_peer_mutex);
            for (auto& peer : m_peers)
            {
                if (peer && peer->connected.load(std::memory_order_acquire))
                    SendPacketLocked(peer->socket, peer->send_mutex, packet.data(), packet.size());
            }
        }

        bool WaitForHostInputs(std::uint32_t frame, InputBundle* bundle)
        {
            const auto started = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(m_input_mutex);
            const bool ready = m_input_cv.wait_for(lock, std::chrono::seconds(RECEIVE_TIMEOUT_SECONDS), [this, frame]() {
                for (std::uint32_t i = 0; i < m_max_players; i++)
                {
                    if (m_player_inputs[i].find(frame) == m_player_inputs[i].end())
                        return m_failed.load(std::memory_order_acquire) || m_stop_requested.load(std::memory_order_acquire);
                }
                return true;
            });
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            LogStall(frame, waited);
            if (!ready || m_failed.load(std::memory_order_acquire))
                return false;
            for (std::uint32_t i = 0; i < m_max_players; i++)
            {
                const auto it = m_player_inputs[i].find(frame);
                if (it == m_player_inputs[i].end())
                    return false;
                (*bundle)[i] = it->second;
            }
            for (std::uint32_t i = m_max_players; i < MAX_PLAYERS; i++)
                (*bundle)[i] = NEUTRAL_FRAME;
            return true;
        }

        bool WaitForBundle(std::uint32_t frame, InputBundle* bundle)
        {
            const auto started = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(m_bundle_mutex);
            const bool ready = m_bundle_cv.wait_for(lock, std::chrono::seconds(RECEIVE_TIMEOUT_SECONDS), [this, frame]() {
                return m_bundles.find(frame) != m_bundles.end() || m_failed.load(std::memory_order_acquire) ||
                    m_stop_requested.load(std::memory_order_acquire) || !m_connected.load(std::memory_order_acquire);
            });
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            LogStall(frame, waited);
            if (!ready)
                return false;
            const auto it = m_bundles.find(frame);
            if (it == m_bundles.end())
                return false;
            *bundle = it->second;
            return true;
        }

        void LogStall(std::uint32_t frame, long long wait_ms)
        {
            const std::uint32_t wait = static_cast<std::uint32_t>(std::max<long long>(0, wait_ms));
            m_last_wait_ms.store(wait, std::memory_order_release);
            if (wait >= 1000)
                Log("STALL severe: poll=%u waited=%ums", static_cast<unsigned>(frame), static_cast<unsigned>(wait));
            else if (wait >= 250)
                Log("STALL: poll=%u waited=%ums", static_cast<unsigned>(frame), static_cast<unsigned>(wait));
        }

        bool BeginPadPoll()
        {
            if (!m_start_committed || !m_connected.load(std::memory_order_acquire))
                return false;
            if (!WaitForFirstPollBarrier())
                return false;

            if (m_have_capture)
            {
                LogLocalInputTransition(m_frame, m_capture);
                if (m_role == Role::Host)
                {
                    std::uint32_t controller = 1;
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        controller = m_players[0].controller;
                    }
                    {
                        std::lock_guard<std::mutex> lock(m_input_mutex);
                        m_player_inputs[controller - 1][m_frame] = m_capture;
                    }
                    m_input_cv.notify_all();
                }
                else if (!SendInputToHost(m_frame, m_capture))
                {
                    Fail("failed to send local controller input");
                    return false;
                }
                ++m_frame;
            }
            else
            {
                m_have_capture = true;
            }

            m_capture = NEUTRAL_FRAME;
            m_output_bundle.fill(NEUTRAL_FRAME);
            const std::uint32_t delay = m_delay.load(std::memory_order_acquire);
            if (m_frame >= delay)
            {
                const std::uint32_t source_frame = m_frame - delay;
                InputBundle bundle{};
                bundle.fill(NEUTRAL_FRAME);
                if (m_role == Role::Host)
                {
                    if (!WaitForHostInputs(source_frame, &bundle))
                    {
                        const std::string reason = "等待房间玩家输入超时，联机无法继续保持同步";
                        BroadcastSessionAbort(reason);
                        Fail(reason.c_str());
                        return false;
                    }
                    BroadcastBundle(source_frame, bundle);
                }
                else if (!WaitForBundle(source_frame, &bundle))
                {
                    const std::string reason = "客户端等待房主权威输入包超时，联机无法继续保持同步";
                    SendControlToHost(ControlType::SessionAbort,
                        reinterpret_cast<const std::uint8_t*>(reason.data()), reason.size());
                    Fail(reason.c_str());
                    return false;
                }
                m_output_bundle = bundle;
                if ((source_frame % LOG_FRAME_INTERVAL) == 0)
                    Log("SYNC poll=%u vsync=%llu delay=%u players=%u wait_ms=%u",
                        static_cast<unsigned>(source_frame), static_cast<unsigned long long>(g_FrameCount),
                        static_cast<unsigned>(delay), static_cast<unsigned>(m_max_players),
                        static_cast<unsigned>(m_last_wait_ms.load(std::memory_order_acquire)));
            }
            PruneInputHistory();
            return true;
        }

        void PruneInputHistory()
        {
            // Keep enough history for large user-selected delays plus jitter/stalls.
            // With the new 100-frame ceiling, the old fixed 120-frame window was too tight.
            const std::uint32_t delay = m_delay.load(std::memory_order_acquire);
            const std::uint32_t keep_window = std::max<std::uint32_t>(240u, delay + 180u);
            if (m_frame < keep_window)
                return;
            const std::uint32_t keep_from = m_frame - keep_window;
            {
                std::lock_guard<std::mutex> lock(m_input_mutex);
                for (auto& map : m_player_inputs)
                {
                    for (auto it = map.begin(); it != map.end();)
                        it = (it->first < keep_from) ? map.erase(it) : std::next(it);
                }
            }
            {
                std::lock_guard<std::mutex> lock(m_bundle_mutex);
                for (auto it = m_bundles.begin(); it != m_bundles.end();)
                    it = (it->first < keep_from) ? m_bundles.erase(it) : std::next(it);
            }
        }

        void HostPeerReceiver(Peer* peer)
        {
            while (!m_stop_requested.load(std::memory_order_acquire) && peer->connected.load(std::memory_order_acquire))
            {
                std::array<std::uint8_t, 4> magic_bytes{};
                if (!ReceiveAll(peer->socket, magic_bytes.data(), magic_bytes.size()))
                    break;
                const std::uint32_t magic = ReadU32(magic_bytes.data());
                if (magic == INPUT_MAGIC)
                {
                    std::array<std::uint8_t, INPUT_REST_SIZE> rest{};
                    if (!ReceiveAll(peer->socket, rest.data(), rest.size()))
                        break;
                    const std::uint32_t frame = ReadU32(rest.data());
                    const std::uint32_t player_id = ReadU32(rest.data() + 4);
                    const std::uint32_t epoch = ReadU32(rest.data() + 8);
                    if (player_id != peer->player_id)
                    {
                        Fail("controller input player id mismatch");
                        break;
                    }
                    if (epoch != m_input_epoch.load(std::memory_order_acquire))
                        continue;
                    std::uint32_t controller = 0;
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        controller = m_players[player_id - 1].controller;
                    }
                    if (controller < 1 || controller > m_max_players)
                        continue;
                    InputFrame input{};
                    std::memcpy(input.data(), rest.data() + 12, input.size());
                    {
                        std::lock_guard<std::mutex> lock(m_input_mutex);
                        m_player_inputs[controller - 1][frame] = input;
                    }
                    m_input_cv.notify_all();
                    continue;
                }
                if (magic == CONTROL_MAGIC)
                {
                    std::array<std::uint8_t, 8> rest{};
                    if (!ReceiveAll(peer->socket, rest.data(), rest.size()))
                        break;
                    const ControlType type = static_cast<ControlType>(ReadU32(rest.data()));
                    const std::uint32_t size = ReadU32(rest.data() + 4);
                    if (size > MAX_CONTROL_PAYLOAD)
                    {
                        Fail("invalid control payload size");
                        break;
                    }
                    std::vector<std::uint8_t> payload(size);
                    if (size > 0 && !ReceiveAll(peer->socket, payload.data(), payload.size()))
                        break;
                    HandleHostControl(*peer, type, payload);
                    continue;
                }
                Fail("invalid packet from client");
                break;
            }

            peer->connected.store(false, std::memory_order_release);
            const std::uint32_t dropped_player_id = peer->player_id;
            bool fatal_disconnect = false;
            bool cancelled_pending_start = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                fatal_disconnect = m_prepare_boot || m_start_committed;
                if (dropped_player_id >= 1 && dropped_player_id <= MAX_PLAYERS)
                    m_players[dropped_player_id - 1] = PlayerState{};

                if (!fatal_disconnect && m_start_requested)
                {
                    ResetStartStateLocked();
                    m_last_error = "有玩家在启动前离开，本次启动已取消；房间仍保持开放";
                    cancelled_pending_start = true;
                }
            }
            Log("P%u disconnected", static_cast<unsigned>(dropped_player_id));
            if (cancelled_pending_start)
                Log("pending synchronized start cancelled; room remains reusable");
            BroadcastRoster();
            if (!m_stop_requested.load(std::memory_order_acquire) && fatal_disconnect)
            {
                const std::string reason = "P" + std::to_string(dropped_player_id) +
                    " 已掉线或关闭联机，所有玩家必须停止本局以避免不同步";
                BroadcastSessionAbort(reason);
                Fail(reason.c_str());
            }
        }

        void ClientReceiverLoop(SOCKET socket)
        {
            while (!m_stop_requested.load(std::memory_order_acquire))
            {
                std::array<std::uint8_t, 4> magic_bytes{};
                if (!ReceiveAll(socket, magic_bytes.data(), magic_bytes.size()))
                    break;
                const std::uint32_t magic = ReadU32(magic_bytes.data());
                if (magic == BUNDLE_MAGIC)
                {
                    std::array<std::uint8_t, BUNDLE_REST_SIZE> rest{};
                    if (!ReceiveAll(socket, rest.data(), rest.size()))
                        break;
                    const std::uint32_t frame = ReadU32(rest.data());
                    const std::uint32_t count = ReadU32(rest.data() + 4);
                    const std::uint32_t epoch = ReadU32(rest.data() + 8);
                    if (count < 1 || count > MAX_PLAYERS)
                    {
                        Fail("invalid authoritative input bundle");
                        break;
                    }
                    if (epoch != m_input_epoch.load(std::memory_order_acquire))
                        continue;
                    InputBundle bundle{};
                    bundle.fill(NEUTRAL_FRAME);
                    for (std::size_t i = 0; i < MAX_PLAYERS; i++)
                        std::memcpy(bundle[i].data(), rest.data() + 12 + i * INPUT_FRAME_BYTES, INPUT_FRAME_BYTES);
                    {
                        std::lock_guard<std::mutex> lock(m_bundle_mutex);
                        m_bundles[frame] = bundle;
                    }
                    m_bundle_cv.notify_all();
                    continue;
                }
                if (magic == CONTROL_MAGIC)
                {
                    std::array<std::uint8_t, 8> rest{};
                    if (!ReceiveAll(socket, rest.data(), rest.size()))
                        break;
                    const ControlType type = static_cast<ControlType>(ReadU32(rest.data()));
                    const std::uint32_t size = ReadU32(rest.data() + 4);
                    if (size > MAX_CONTROL_PAYLOAD)
                    {
                        Fail("invalid control payload size");
                        break;
                    }
                    std::vector<std::uint8_t> payload(size);
                    if (size > 0 && !ReceiveAll(socket, payload.data(), payload.size()))
                        break;
                    HandleClientControl(type, payload);
                    continue;
                }
                Fail("invalid packet from host");
                break;
            }
            m_connected.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            m_input_cv.notify_all();
            m_bundle_cv.notify_all();
            m_boot_cv.notify_all();
            m_first_poll_cv.notify_all();
            if (!m_stop_requested.load(std::memory_order_acquire) && !m_failed.load(std::memory_order_acquire))
            {
                bool active_round = false;
                {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    active_round = m_prepare_boot || m_start_committed;
                }
                if (active_round)
                    Fail("host disconnected during synchronized session");
                else
                {
                    SetLastError("与房主的连接中断，正在自动重新连接");
                    Log("host disconnected before synchronized boot; reconnect will be attempted");
                    m_connecting.store(true, std::memory_order_release);
                }
            }
        }

        void HandleHostControl(Peer& peer, ControlType type, const std::vector<std::uint8_t>& payload)
        {
            switch (type)
            {
                case ControlType::GameMatch:
                {
                    if (payload.size() != 8)
                        return;
                    const std::uint32_t id = ReadU32(payload.data());
                    const bool matched = ReadU32(payload.data() + 4) != 0;
                    if (id != peer.player_id)
                        return;
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        m_players[id - 1].game_match = matched;
                    }
                    Log("P%u GAME_MATCH=%s", static_cast<unsigned>(id), matched ? "yes" : "no");
                    BroadcastRoster();
                    MaybeAdvanceStart();
                    break;
                }
                case ControlType::MemcardReady:
                {
                    if (payload.size() != 12)
                        return;
                    const bool ok = ReadU32(payload.data()) != 0;
                    const std::uint64_t hash = ReadU64(payload.data() + 4);
                    bool accepted = false;
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        accepted = ok && (hash == m_memory_card_crc);
                        m_players[peer.player_id - 1].memcard_ready = accepted;
                    }
                    Log("P%u MEMCARD_READY=%s hash=%016llX", static_cast<unsigned>(peer.player_id),
                        accepted ? "yes" : "no", static_cast<unsigned long long>(hash));
                    if (!accepted)
                    {
                        CancelPendingStart("有玩家的记忆卡同步校验失败，本次启动已取消，可直接重试");
                        break;
                    }
                    BroadcastRoster();
                    MaybeAdvanceStart();
                    break;
                }
                case ControlType::BootReady:
                {
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        m_players[peer.player_id - 1].boot_ready = true;
                    }
                    Log("P%u BOOT_READY", static_cast<unsigned>(peer.player_id));
                    BroadcastRoster();
                    MaybeCommitStart();
                    break;
                }
                case ControlType::FirstPollReady:
                {
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        m_players[peer.player_id - 1].first_poll_ready = true;
                    }
                    Log("P%u FIRST_POLL_READY", static_cast<unsigned>(peer.player_id));
                    MaybeReleaseFirstPoll();
                    break;
                }
                case ControlType::SessionAbort:
                {
                    const std::string reason(payload.empty() ?
                        ("P" + std::to_string(peer.player_id) + " 请求终止本次联机") :
                        std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
                    Log("P%u requested SESSION_ABORT: %s",
                        static_cast<unsigned>(peer.player_id), reason.c_str());
                    BroadcastSessionAbort(reason);
                    Fail(reason.c_str());
                    break;
                }
                case ControlType::RuntimeRequest:
                {
                    if (payload.size() == 4)
                        HandleRuntimeRequest(peer.player_id, ReadU32(payload.data()));
                    break;
                }
                case ControlType::RuntimeAck:
                {
                    if (payload.size() != 4)
                        break;
                    const std::uint32_t change_id = ReadU32(payload.data());
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        if (m_runtime_reconfiguring && change_id == m_runtime_change_id)
                            m_runtime_acks[peer.player_id - 1] = true;
                    }
                    MaybeCommitRuntimeConfig();
                    break;
                }
                default:
                    break;
            }
        }

        void HandleClientControl(ControlType type, const std::vector<std::uint8_t>& payload)
        {
            switch (type)
            {
                case ControlType::Roster:
                    HandleRoster(payload);
                    break;
                case ControlType::GameManifest:
                    HandleGameManifest(payload);
                    break;
                case ControlType::MemcardBegin:
                    HandleClientMemcardBegin(payload);
                    break;
                case ControlType::MemcardChunk:
                    HandleClientMemcardChunk(payload);
                    break;
                case ControlType::MemcardEnd:
                    HandleClientMemcardEnd();
                    break;
                case ControlType::PrepareBoot:
                    HandleClientPrepareBoot();
                    break;
                case ControlType::StartCommit:
                {
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        m_start_committed = true;
                    }
                    Log("START_COMMIT received");
                    m_boot_cv.notify_all();
                    break;
                }
                case ControlType::FirstPollGo:
                {
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        m_first_poll_go = true;
                    }
                    Log("FIRST_POLL_GO received");
                    m_first_poll_cv.notify_all();
                    break;
                }
                case ControlType::SessionAbort:
                {
                    const std::string reason(payload.empty() ? "房主终止了本次联机" :
                        std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
                    Log("SESSION_ABORT received: %s", reason.c_str());
                    Fail(reason.c_str());
                    break;
                }
                case ControlType::RuntimeApply:
                {
                    std::uint32_t change_id = 0;
                    std::uint32_t delay = 0;
                    std::uint32_t topology = 0;
                    std::array<std::uint32_t, MAX_PLAYERS> controllers{};
                    if (!ParseRuntimeConfig(payload, &change_id, &delay, &topology, &controllers) ||
                        !ApplyRuntimeConfigLocal(change_id, delay, topology, controllers))
                    {
                        Fail("invalid runtime Netplay configuration");
                        break;
                    }
                    std::array<std::uint8_t, 4> ack{};
                    WriteU32(ack.data(), change_id);
                    if (!SendControlToHost(ControlType::RuntimeAck, ack.data(), static_cast<std::uint32_t>(ack.size())))
                        Fail("failed to acknowledge runtime Netplay configuration");
                    break;
                }
                case ControlType::RuntimeCommit:
                {
                    if (payload.size() == 4)
                        FinishRuntimeConfig(ReadU32(payload.data()));
                    break;
                }
                default:
                    break;
            }
        }

        void Fail(const char* message)
        {
            SetLastError(message);
            const bool first_failure = !m_failed.exchange(true, std::memory_order_acq_rel);
            if (first_failure)
            {
                Log("ERROR: %s", message);
                NotifyFatalSessionFailure(message);
            }
            m_connected.store(false, std::memory_order_release);
            m_connecting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            m_input_cv.notify_all();
            m_bundle_cv.notify_all();
            m_boot_cv.notify_all();
            m_first_poll_cv.notify_all();
        }

        Role m_role = Role::Disabled;
        std::string m_username;
        std::string m_host = "127.0.0.1";
        std::uint16_t m_port = DEFAULT_PORT;
        std::uint32_t m_max_players = 2;
        std::uint32_t m_local_player_id = 0;
        std::uint64_t m_session_id = 0;
        std::atomic<std::uint32_t> m_delay{2};
        std::atomic<std::uint32_t> m_topology_mode{1};
        std::atomic<std::uint32_t> m_input_epoch{0};
        bool m_memory_card_sync_enabled = true;

        std::array<PlayerState, MAX_PLAYERS> m_players{};
        bool m_runtime_reconfiguring = false;
        bool m_resume_after_reconfig = false;
        std::uint32_t m_runtime_change_counter = 0;
        std::uint32_t m_runtime_change_id = 0;
        std::array<bool, MAX_PLAYERS> m_runtime_acks{};
        bool m_game_selected = false;
        bool m_local_game_match = false;
        std::string m_game_title;
        std::string m_game_serial;
        std::uint32_t m_game_crc = 0;
        std::string m_local_game_path;

        bool m_start_requested = false;
        bool m_memcard_transfer_started = false;
        bool m_memory_card_local_ready = false;
        bool m_memory_card_present = false;
        std::uint64_t m_memory_card_crc = 0;
        std::uint32_t m_memory_card_size = 0;
        std::string m_memory_card_status = "等待记忆卡同步";
        std::string m_shadow_card_filename;
        std::vector<std::uint8_t> m_memcard_receive;
        std::uint64_t m_memcard_received_bytes = 0;

        bool m_prepare_boot = false;
        bool m_boot_launch_pending = false;
        bool m_boot_launch_consumed = false;
        bool m_local_boot_ready = false;
        bool m_start_committed = false;
        bool m_first_poll_go = false;

        std::uint32_t m_frame = 0;
        bool m_have_capture = false;
        InputFrame m_capture = NEUTRAL_FRAME;
        InputFrame m_last_logged_local_input = NEUTRAL_FRAME;
        InputBundle m_output_bundle = {NEUTRAL_FRAME, NEUTRAL_FRAME, NEUTRAL_FRAME, NEUTRAL_FRAME};
        std::array<std::unordered_map<std::uint32_t, InputFrame>, MAX_PLAYERS> m_player_inputs;
        std::unordered_map<std::uint32_t, InputBundle> m_bundles;

        bool m_winsock_started = false;
        std::atomic<SOCKET> m_listener{INVALID_SOCKET};
        SOCKET m_client_socket = INVALID_SOCKET;
        std::array<std::unique_ptr<Peer>, 3> m_peers{};
        std::atomic<bool> m_connected{false};
        std::atomic<bool> m_connecting{false};
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_failed{false};
        std::atomic<bool> m_stop_requested{false};
        std::atomic<bool> m_connect_worker_started{false};
        std::atomic<std::uint32_t> m_last_wait_ms{0};
        std::thread m_connector;

        mutable std::mutex m_state_mutex;
        std::mutex m_peer_mutex;
        std::mutex m_client_socket_mutex;
        std::mutex m_client_send_mutex;
        std::mutex m_input_mutex;
        std::mutex m_bundle_mutex;
        std::condition_variable m_input_cv;
        std::condition_variable m_bundle_cv;
        std::condition_variable m_boot_cv;
        std::condition_variable m_first_poll_cv;
        std::string m_last_error;
        std::string m_log_path;

        mutable std::mutex m_log_mutex;
        mutable FILE* m_log_file = nullptr;
        const std::chrono::steady_clock::time_point m_session_started = std::chrono::steady_clock::now();
    };

    std::unique_ptr<Session>& GetSessionStorage()
    {
        static std::unique_ptr<Session> session = std::make_unique<Session>();
        return session;
    }

    std::mutex& GetSessionLifecycleMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    Session& GetSession()
    {
        std::unique_ptr<Session>& session = GetSessionStorage();
        if (!session)
            session = std::make_unique<Session>();
        return *session;
    }

    bool RestartSessionImpl()
    {
        std::lock_guard<std::mutex> lock(GetSessionLifecycleMutex());
        std::unique_ptr<Session>& session = GetSessionStorage();
        session.reset();
        session = std::make_unique<Session>();
        if (!session->IsConfigured())
            return false;
        session->StartSessionAsync();
        return true;
    }
} // namespace

bool IsCustomBuild() { return true; }
bool IsConfigured() { return GetSession().IsConfigured(); }
void StartSessionAsync() { GetSession().StartSessionAsync(); }
bool RestartSession() { return RestartSessionImpl(); }
StatusSnapshot GetStatusSnapshot() { return GetSession().GetStatusSnapshot(); }
bool HostSelectGame(const std::string& path, const std::string& title, const std::string& serial, std::uint32_t crc)
{
    return GetSession().HostSelectGame(path, title, serial, crc);
}
bool RequestSynchronizedBoot() { return GetSession().RequestSynchronizedBoot(); }
bool ConsumeBootLaunchRequest(std::string* path) { return GetSession().ConsumeBootLaunchRequest(path); }
bool RequestRuntimeSettings(std::uint32_t local_controller, std::uint32_t delay, std::uint32_t topology_mode)
{
    return GetSession().RequestRuntimeSettings(local_controller, delay, topology_mode);
}
bool CanStartVM() { return GetSession().CanStartVM(); }
bool ShouldHoldBootBarrier() { return GetSession().ShouldHoldBootBarrier(); }
void NotifyBootReady() { GetSession().NotifyBootReady(); }
bool WaitForStartCommit() { return GetSession().WaitForStartCommit(); }
void ApplyDeterministicConfig() { GetSession().ApplyDeterministicConfig(); }
bool ShouldForceDualShock2Slot(std::uint32_t slot) { return GetSession().ShouldForceDualShock2Slot(slot); }
bool ShouldDisconnectControllerSlot(std::uint32_t slot) { return GetSession().ShouldDisconnectControllerSlot(slot); }
std::uint8_t HandlePadResponse(std::uint8_t slot, std::uint32_t index, std::uint8_t local_value)
{
    return GetSession().HandlePadResponse(slot, index, local_value);
}
void Shutdown() { GetSession().Stop(); }
#else
bool IsCustomBuild() { return true; }
bool IsConfigured() { return false; }
void StartSessionAsync() {}
bool RestartSession() { return false; }
StatusSnapshot GetStatusSnapshot() { return {}; }
bool HostSelectGame(const std::string&, const std::string&, const std::string&, std::uint32_t) { return false; }
bool RequestSynchronizedBoot() { return false; }
bool ConsumeBootLaunchRequest(std::string*) { return false; }
bool RequestRuntimeSettings(std::uint32_t, std::uint32_t, std::uint32_t) { return false; }
bool CanStartVM() { return true; }
bool ShouldHoldBootBarrier() { return false; }
void NotifyBootReady() {}
bool WaitForStartCommit() { return true; }
void ApplyDeterministicConfig() {}
bool ShouldForceDualShock2Slot(std::uint32_t) { return false; }
bool ShouldDisconnectControllerSlot(std::uint32_t) { return false; }
std::uint8_t HandlePadResponse(std::uint8_t, std::uint32_t, std::uint8_t local_value) { return local_value; }
void Shutdown() {}
#endif
} // namespace ModernNetplay
