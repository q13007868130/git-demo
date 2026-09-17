// SPDX-FileCopyrightText: 2026 PCSX2 Modern Netplay Port contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Netplay/ModernNetplay.h"

#include "Counters.h"
#include "GameList.h"

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
#include <mutex>
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
    using InputFrame = std::array<std::uint8_t, 6>;

    constexpr std::uint32_t HELLO_MAGIC = 0x50324E50;   // P2NP
    constexpr std::uint32_t FRAME_MAGIC = 0x46524D45;   // FRME
    constexpr std::uint32_t CONTROL_MAGIC = 0x43544C31; // CTL1
    constexpr std::uint32_t PROTOCOL_VERSION = 2;
    constexpr std::uint16_t DEFAULT_PORT = 27886;
    constexpr int RECEIVE_TIMEOUT_SECONDS = 30;
    constexpr int BOOT_BARRIER_TIMEOUT_SECONDS = 60;
    constexpr std::uint32_t LOG_FRAME_INTERVAL = 120;
    constexpr std::uint32_t MAX_CONTROL_PAYLOAD = 4096;
    constexpr InputFrame NEUTRAL_FRAME = {0xff, 0xff, 0x7f, 0x7f, 0x7f, 0x7f};

    enum class Role : std::uint32_t
    {
        Disabled = 0,
        Host = 1,
        Client = 2,
    };

    enum class ControlType : std::uint32_t
    {
        GameManifest = 1,
        GameMatch = 2,
        PrepareBoot = 3,
        BootReady = 4,
        StartCommit = 5,
        FirstPollReady = 6,
        FirstPollGo = 7,
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

    class Session final
    {
    public:
        Session()
        {
            ReadConfiguration();
            if (m_role != Role::Disabled)
            {
                OpenLog();
                Log("session configured: role=%s host=%s port=%u delay=%u protocol=%u",
                    RoleName(), m_host.c_str(), static_cast<unsigned>(m_port),
                    static_cast<unsigned>(m_delay.load()), static_cast<unsigned>(PROTOCOL_VERSION));
                Log("v0.5a boot-barrier preview: strict game manifest + synchronized VM start + first-poll barrier");
            }
        }

        ~Session()
        {
            Stop();
        }

        bool IsConfigured() const
        {
            return m_role != Role::Disabled;
        }

        void StartSessionAsync()
        {
            if (!IsConfigured() || m_connected.load(std::memory_order_acquire) ||
                m_failed.load(std::memory_order_acquire) || m_stop_requested.load(std::memory_order_acquire))
                return;

            bool expected = false;
            if (!m_connect_worker_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                return;

            Log("lobby connection worker started");
            m_connector = std::thread([this]() { EnsureConnected(); });
        }

        StatusSnapshot GetStatusSnapshot() const
        {
            StatusSnapshot out;
            out.configured = IsConfigured();
            out.connecting = m_connecting.load(std::memory_order_acquire);
            out.connected = m_connected.load(std::memory_order_acquire);
            out.failed = m_failed.load(std::memory_order_acquire);
            out.player_count = out.configured ? (out.connected ? 2u : 1u) : 0u;
            out.delay = m_delay.load(std::memory_order_acquire);
            out.port = m_port;
            out.role = RoleName();
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                out.peer = m_peer;
                out.last_error = m_last_error;
                out.log_path = m_log_path;
                out.game_selected = m_game_selected;
                out.local_game_match = m_local_game_match;
                out.peer_game_match = m_peer_game_match;
                out.prepare_boot = m_prepare_boot;
                out.local_boot_ready = m_local_boot_ready;
                out.peer_boot_ready = m_peer_boot_ready;
                out.start_committed = m_start_committed;
                out.first_poll_released = m_first_poll_go;
                out.game_title = m_game_title;
                out.game_serial = m_game_serial;
                out.game_crc = m_game_crc;
                out.local_game_path = m_local_game_path;
            }
            return out;
        }

        bool HostSelectGame(const std::string& path, const std::string& title, const std::string& serial, std::uint32_t crc)
        {
            if (m_role != Role::Host || path.empty() || serial.empty() || crc == 0)
                return false;

            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (m_prepare_boot)
                    return false;
                m_game_selected = true;
                m_local_game_match = true;
                m_peer_game_match = false;
                m_game_title = title;
                m_game_serial = serial;
                m_game_crc = crc;
                m_local_game_path = path;
                ResetBootStateLocked();
            }

            Log("host selected game: title=%s serial=%s crc=%08X", title.c_str(), serial.c_str(), crc);
            if (m_connected.load(std::memory_order_acquire) && !SendGameManifest())
            {
                Fail("failed to send game manifest");
                return false;
            }
            return true;
        }

        bool RequestSynchronizedBoot()
        {
            if (m_role != Role::Host || !m_connected.load(std::memory_order_acquire))
                return false;

            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!m_game_selected || !m_local_game_match || !m_peer_game_match || m_prepare_boot)
                    return false;
                ResetBootStateLocked();
                m_prepare_boot = true;
                m_boot_launch_pending = true;
            }

            if (!SendControl(ControlType::PrepareBoot, nullptr, 0))
            {
                Fail("failed to send PREPARE_BOOT");
                return false;
            }

            Log("PREPARE_BOOT sent; both VMs must initialize and stop at boot barrier");
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
            bool send_ready = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!m_prepare_boot || m_local_boot_ready)
                    return;
                m_local_boot_ready = true;
                send_ready = true;
            }

            if (send_ready && !SendControl(ControlType::BootReady, nullptr, 0))
            {
                Fail("failed to send BOOT_READY");
                return;
            }

            Log("BOOT_READY: local VM initialized and waiting");
            MaybeCommitStart();
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

        std::uint8_t HandlePadResponse(std::uint8_t unified_slot, std::uint32_t command_index, std::uint8_t local_value)
        {
            if (m_role == Role::Disabled || command_index < 3 || command_index > 8)
                return local_value;

            const std::size_t input_index = static_cast<std::size_t>(command_index - 3);

            if (unified_slot == 0 && command_index == 3)
            {
                if (!BeginPadPoll())
                    return local_value;
            }

            if (!m_connected.load(std::memory_order_acquire))
                return local_value;

            if (unified_slot == 0)
            {
                m_capture[input_index] = local_value;
                return (m_role == Role::Host) ? m_local_output[input_index] : m_remote_output[input_index];
            }

            if (unified_slot == 1)
                return (m_role == Role::Host) ? m_remote_output[input_index] : m_local_output[input_index];

            return local_value;
        }

        void Stop()
        {
            if (m_stop_requested.exchange(true, std::memory_order_acq_rel))
                return;

            Log("session shutdown requested");
            m_running.store(false, std::memory_order_release);
            m_connected.store(false, std::memory_order_release);
            m_connecting.store(false, std::memory_order_release);
            m_remote_cv.notify_all();
            m_boot_cv.notify_all();
            m_first_poll_cv.notify_all();

            const SOCKET listener = m_listener.exchange(INVALID_SOCKET, std::memory_order_acq_rel);
            if (listener != INVALID_SOCKET)
                closesocket(listener);

            SOCKET socket = INVALID_SOCKET;
            {
                std::lock_guard<std::mutex> lock(m_socket_mutex);
                socket = m_socket;
                m_socket = INVALID_SOCKET;
            }
            if (socket != INVALID_SOCKET)
            {
                shutdown(socket, SD_BOTH);
                closesocket(socket);
            }

            if (m_connector.joinable() && m_connector.get_id() != std::this_thread::get_id())
                m_connector.join();
            if (m_receiver.joinable() && m_receiver.get_id() != std::this_thread::get_id())
                m_receiver.join();

            if (m_winsock_started)
            {
                WSACleanup();
                m_winsock_started = false;
            }

            Log("session stopped");
            CloseLog();
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

            if (const char* host = std::getenv("PCSX2_NETPLAY_HOST"))
                m_host = host;

            if (const char* port = std::getenv("PCSX2_NETPLAY_PORT"))
            {
                const int value = std::atoi(port);
                if (value > 0 && value <= 65535)
                    m_port = static_cast<std::uint16_t>(value);
            }

            if (const char* delay = std::getenv("PCSX2_NETPLAY_DELAY"))
                m_delay.store(static_cast<std::uint32_t>(std::clamp(std::atoi(delay), 1, 12)), std::memory_order_release);
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

            std::filesystem::path absolute_path = std::filesystem::absolute(file_path, ec);
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

        void ResetBootStateLocked()
        {
            m_prepare_boot = false;
            m_boot_launch_pending = false;
            m_boot_launch_consumed = false;
            m_local_boot_ready = false;
            m_peer_boot_ready = false;
            m_start_committed = false;
            m_local_first_poll_ready = false;
            m_peer_first_poll_ready = false;
            m_first_poll_go = false;
            m_frame = 0;
            m_have_capture = false;
            m_capture = NEUTRAL_FRAME;
            m_local_output = NEUTRAL_FRAME;
            m_remote_output = NEUTRAL_FRAME;
            m_local_frames.clear();
            {
                std::lock_guard<std::mutex> remote_lock(m_remote_mutex);
                m_remote_frames.clear();
            }
        }

        bool BeginPadPoll()
        {
            if (!EnsureConnected())
                return false;

            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (m_prepare_boot && !m_start_committed)
                    return false;
            }

            if (!WaitForFirstPollBarrier())
                return false;

            if (m_have_capture)
            {
                m_local_frames[m_frame] = m_capture;
                if (!SendFrame(m_frame, m_capture))
                {
                    Fail("failed to send controller frame");
                    return false;
                }
                ++m_frame;
            }
            else
            {
                m_have_capture = true;
            }

            m_capture = NEUTRAL_FRAME;
            m_local_output = NEUTRAL_FRAME;
            m_remote_output = NEUTRAL_FRAME;

            if (m_frame >= m_delay.load(std::memory_order_acquire))
            {
                const std::uint32_t source_frame = m_frame - m_delay.load(std::memory_order_acquire);
                const auto local = m_local_frames.find(source_frame);
                if (local == m_local_frames.end())
                {
                    Fail("local delayed frame is missing");
                    return false;
                }
                m_local_output = local->second;

                if (!WaitForRemoteFrame(source_frame, &m_remote_output))
                {
                    Fail("timed out waiting for peer controller frame");
                    return false;
                }

                if ((source_frame % LOG_FRAME_INTERVAL) == 0)
                    LogFrameSummary(source_frame, m_local_output, m_remote_output);
            }

            PruneFrames();
            return true;
        }

        bool WaitForFirstPollBarrier()
        {
            bool send_ready = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!m_prepare_boot)
                    return true;
                if (m_first_poll_go)
                    return true;
                if (!m_local_first_poll_ready)
                {
                    m_local_first_poll_ready = true;
                    send_ready = true;
                }
            }

            if (send_ready)
            {
                Log("FIRST_POLL_READY: local VM reached first synchronized DS2 poll");
                if (!SendControl(ControlType::FirstPollReady, nullptr, 0))
                {
                    Fail("failed to send FIRST_POLL_READY");
                    return false;
                }
            }

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

        void LogFrameSummary(std::uint32_t frame, const InputFrame& local, const InputFrame& remote)
        {
            Log("SYNC poll=%u vsync=%llu delay=%u wait_ms=%u local=%02x%02x%02x%02x%02x%02x remote=%02x%02x%02x%02x%02x%02x",
                static_cast<unsigned>(frame), static_cast<unsigned long long>(g_FrameCount),
                static_cast<unsigned>(m_delay.load(std::memory_order_acquire)),
                static_cast<unsigned>(m_last_wait_ms.load(std::memory_order_acquire)),
                static_cast<unsigned>(local[0]), static_cast<unsigned>(local[1]), static_cast<unsigned>(local[2]),
                static_cast<unsigned>(local[3]), static_cast<unsigned>(local[4]), static_cast<unsigned>(local[5]),
                static_cast<unsigned>(remote[0]), static_cast<unsigned>(remote[1]), static_cast<unsigned>(remote[2]),
                static_cast<unsigned>(remote[3]), static_cast<unsigned>(remote[4]), static_cast<unsigned>(remote[5]));
        }

        bool EnsureConnected()
        {
            if (m_connected.load(std::memory_order_acquire))
                return true;
            if (m_failed.load(std::memory_order_acquire) || m_stop_requested.load(std::memory_order_acquire))
                return false;

            std::lock_guard<std::mutex> connect_lock(m_connect_mutex);
            if (m_connected.load(std::memory_order_acquire))
                return true;
            if (m_failed.load(std::memory_order_acquire) || m_stop_requested.load(std::memory_order_acquire))
                return false;

            m_connecting.store(true, std::memory_order_release);

            if (!m_winsock_started)
            {
                WSADATA data{};
                if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                {
                    m_connecting.store(false, std::memory_order_release);
                    Fail("WSAStartup failed");
                    return false;
                }
                m_winsock_started = true;
            }

            SOCKET socket = (m_role == Role::Host) ? AcceptPeer() : ConnectToHost();
            if (socket == INVALID_SOCKET)
            {
                m_connecting.store(false, std::memory_order_release);
                if (!m_stop_requested.load(std::memory_order_acquire))
                    Fail("could not establish TCP connection");
                return false;
            }

            BOOL no_delay = TRUE;
            setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));

            if (!ExchangeHello(socket))
            {
                closesocket(socket);
                m_connecting.store(false, std::memory_order_release);
                if (!m_stop_requested.load(std::memory_order_acquire))
                    Fail("Netplay handshake failed");
                return false;
            }

            {
                std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
                m_socket = socket;
            }
            m_running.store(true, std::memory_order_release);
            m_connected.store(true, std::memory_order_release);
            m_connecting.store(false, std::memory_order_release);
            m_receiver = std::thread([this]() { ReceiverLoop(); });

            Log("peer connected: protocol=%u delay=%u peer=%s",
                static_cast<unsigned>(PROTOCOL_VERSION), static_cast<unsigned>(m_delay.load()),
                GetStatusSnapshot().peer.c_str());

            if (m_role == Role::Host)
            {
                bool selected = false;
                {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    selected = m_game_selected;
                }
                if (selected && !SendGameManifest())
                {
                    Fail("failed to announce selected game after connect");
                    return false;
                }
            }

            return true;
        }

        SOCKET AcceptPeer()
        {
            SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener == INVALID_SOCKET)
                return INVALID_SOCKET;

            m_listener.store(listener, std::memory_order_release);

            BOOL reuse = TRUE;
            setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_ANY);
            address.sin_port = htons(m_port);

            if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
                listen(listener, 1) == SOCKET_ERROR)
            {
                if (m_listener.exchange(INVALID_SOCKET, std::memory_order_acq_rel) == listener)
                    closesocket(listener);
                return INVALID_SOCKET;
            }

            Log("room open: TCP port %u, players=1/2, waiting for peer", static_cast<unsigned>(m_port));

            SOCKET peer = INVALID_SOCKET;
            while (!m_stop_requested.load(std::memory_order_acquire))
            {
                fd_set read_set;
                FD_ZERO(&read_set);
                FD_SET(listener, &read_set);
                timeval timeout{};
                timeout.tv_sec = 0;
                timeout.tv_usec = 250000;
                const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
                if (ready > 0 && FD_ISSET(listener, &read_set))
                {
                    sockaddr_storage peer_address{};
                    int peer_address_length = sizeof(peer_address);
                    peer = accept(listener, reinterpret_cast<sockaddr*>(&peer_address), &peer_address_length);
                    if (peer != INVALID_SOCKET)
                    {
                        char host[NI_MAXHOST]{};
                        if (getnameinfo(reinterpret_cast<const sockaddr*>(&peer_address), peer_address_length,
                            host, sizeof(host), nullptr, 0, NI_NUMERICHOST) == 0)
                        {
                            std::lock_guard<std::mutex> lock(m_state_mutex);
                            m_peer = host;
                        }
                        break;
                    }
                }
                else if (ready == SOCKET_ERROR)
                {
                    break;
                }
            }

            if (m_listener.exchange(INVALID_SOCKET, std::memory_order_acq_rel) == listener)
                closesocket(listener);
            return peer;
        }

        SOCKET ConnectToHost()
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
                        if (connect(candidate, current->ai_addr, static_cast<int>(current->ai_addrlen)) != SOCKET_ERROR)
                        {
                            connected = candidate;
                            break;
                        }
                        closesocket(candidate);
                    }
                    freeaddrinfo(result);
                    if (connected != INVALID_SOCKET)
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        m_peer = m_host;
                        return connected;
                    }
                }

                if (attempt == 1 || (attempt % 10) == 0)
                    Log("joining room: %s:%u, attempt=%u", m_host.c_str(), static_cast<unsigned>(m_port), attempt);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            return INVALID_SOCKET;
        }

        bool ExchangeHello(SOCKET socket)
        {
            std::array<std::uint8_t, 16> outgoing{};
            WriteU32(outgoing.data() + 0, HELLO_MAGIC);
            WriteU32(outgoing.data() + 4, PROTOCOL_VERSION);
            WriteU32(outgoing.data() + 8, static_cast<std::uint32_t>(m_role));
            WriteU32(outgoing.data() + 12, m_delay.load(std::memory_order_acquire));

            std::array<std::uint8_t, 16> incoming{};
            if (m_role == Role::Host)
            {
                if (!ReceiveAll(socket, incoming.data(), incoming.size()) || !ValidateHello(incoming, Role::Client))
                    return false;
                return SendAll(socket, outgoing.data(), outgoing.size());
            }

            if (!SendAll(socket, outgoing.data(), outgoing.size()) || !ReceiveAll(socket, incoming.data(), incoming.size()) ||
                !ValidateHello(incoming, Role::Host))
                return false;

            m_delay.store(std::clamp<std::uint32_t>(ReadU32(incoming.data() + 12), 1, 12), std::memory_order_release);
            return true;
        }

        bool ValidateHello(const std::array<std::uint8_t, 16>& hello, Role expected_role) const
        {
            return ReadU32(hello.data() + 0) == HELLO_MAGIC && ReadU32(hello.data() + 4) == PROTOCOL_VERSION &&
                ReadU32(hello.data() + 8) == static_cast<std::uint32_t>(expected_role);
        }

        bool SendFrame(std::uint32_t frame, const InputFrame& input)
        {
            std::array<std::uint8_t, 14> packet{};
            WriteU32(packet.data() + 0, FRAME_MAGIC);
            WriteU32(packet.data() + 4, frame);
            std::memcpy(packet.data() + 8, input.data(), input.size());

            std::lock_guard<std::mutex> lock(m_socket_mutex);
            return m_socket != INVALID_SOCKET && SendAll(m_socket, packet.data(), packet.size());
        }

        bool SendControl(ControlType type, const void* payload, std::uint32_t size)
        {
            if (size > MAX_CONTROL_PAYLOAD)
                return false;

            std::array<std::uint8_t, 12> header{};
            WriteU32(header.data() + 0, CONTROL_MAGIC);
            WriteU32(header.data() + 4, static_cast<std::uint32_t>(type));
            WriteU32(header.data() + 8, size);

            std::lock_guard<std::mutex> lock(m_socket_mutex);
            if (m_socket == INVALID_SOCKET || !SendAll(m_socket, header.data(), header.size()))
                return false;
            return size == 0 || SendAll(m_socket, payload, size);
        }

        bool SendGameManifest()
        {
            std::array<std::uint8_t, 4 + 32 + 160> payload{};
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!m_game_selected)
                    return false;
                WriteU32(payload.data(), m_game_crc);
                std::snprintf(reinterpret_cast<char*>(payload.data() + 4), 32, "%s", m_game_serial.c_str());
                std::snprintf(reinterpret_cast<char*>(payload.data() + 36), 160, "%s", m_game_title.c_str());
            }
            Log("GAME_MANIFEST sent");
            return SendControl(ControlType::GameManifest, payload.data(), static_cast<std::uint32_t>(payload.size()));
        }

        void ReceiverLoop()
        {
            while (m_running.load(std::memory_order_acquire) && !m_stop_requested.load(std::memory_order_acquire))
            {
                SOCKET socket;
                {
                    std::lock_guard<std::mutex> lock(m_socket_mutex);
                    socket = m_socket;
                }
                if (socket == INVALID_SOCKET)
                    break;

                std::array<std::uint8_t, 4> magic_bytes{};
                if (!ReceiveAll(socket, magic_bytes.data(), magic_bytes.size()))
                    break;

                const std::uint32_t magic = ReadU32(magic_bytes.data());
                if (magic == FRAME_MAGIC)
                {
                    std::array<std::uint8_t, 10> rest{};
                    if (!ReceiveAll(socket, rest.data(), rest.size()))
                        break;

                    InputFrame input{};
                    const std::uint32_t frame = ReadU32(rest.data());
                    std::memcpy(input.data(), rest.data() + 4, input.size());
                    {
                        std::lock_guard<std::mutex> lock(m_remote_mutex);
                        m_remote_frames[frame] = input;
                    }
                    m_remote_cv.notify_all();
                    continue;
                }

                if (magic == CONTROL_MAGIC)
                {
                    std::array<std::uint8_t, 8> header_rest{};
                    if (!ReceiveAll(socket, header_rest.data(), header_rest.size()))
                        break;
                    const ControlType type = static_cast<ControlType>(ReadU32(header_rest.data()));
                    const std::uint32_t size = ReadU32(header_rest.data() + 4);
                    if (size > MAX_CONTROL_PAYLOAD)
                    {
                        Fail("invalid Netplay control payload size");
                        break;
                    }
                    std::vector<std::uint8_t> payload(size);
                    if (size > 0 && !ReceiveAll(socket, payload.data(), payload.size()))
                        break;
                    HandleControl(type, payload);
                    continue;
                }

                Fail("invalid Netplay packet magic");
                break;
            }

            m_connected.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            m_remote_cv.notify_all();
            m_boot_cv.notify_all();
            m_first_poll_cv.notify_all();
            if (!m_stop_requested.load(std::memory_order_acquire) && !m_failed.load(std::memory_order_acquire))
            {
                SetLastError("peer disconnected");
                Log("peer disconnected");
            }
        }

        void HandleControl(ControlType type, const std::vector<std::uint8_t>& payload)
        {
            switch (type)
            {
                case ControlType::GameManifest:
                    HandleGameManifest(payload);
                    break;
                case ControlType::GameMatch:
                    HandleGameMatch(payload);
                    break;
                case ControlType::PrepareBoot:
                    HandlePrepareBoot();
                    break;
                case ControlType::BootReady:
                {
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        m_peer_boot_ready = true;
                    }
                    Log("BOOT_READY received from peer");
                    MaybeCommitStart();
                    m_boot_cv.notify_all();
                    break;
                }
                case ControlType::StartCommit:
                {
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        m_start_committed = true;
                    }
                    Log("START_COMMIT received from host");
                    m_boot_cv.notify_all();
                    break;
                }
                case ControlType::FirstPollReady:
                {
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        m_peer_first_poll_ready = true;
                    }
                    Log("FIRST_POLL_READY received from peer");
                    MaybeReleaseFirstPoll();
                    m_first_poll_cv.notify_all();
                    break;
                }
                case ControlType::FirstPollGo:
                {
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        m_first_poll_go = true;
                    }
                    Log("FIRST_POLL_GO received from host");
                    m_first_poll_cv.notify_all();
                    break;
                }
                default:
                    Fail("unknown Netplay control message");
                    break;
            }
        }

        void HandleGameManifest(const std::vector<std::uint8_t>& payload)
        {
            if (m_role != Role::Client || payload.size() != (4 + 32 + 160))
            {
                Fail("invalid GAME_MANIFEST");
                return;
            }

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
                m_peer_game_match = true;
                ResetBootStateLocked();
            }

            std::array<std::uint8_t, 4> match_payload{};
            WriteU32(match_payload.data(), matched ? 1u : 0u);
            Log("GAME_MANIFEST received: title=%s serial=%s crc=%08X local_match=%s",
                title.c_str(), serial.c_str(), crc, matched ? "yes" : "no");
            if (!SendControl(ControlType::GameMatch, match_payload.data(), static_cast<std::uint32_t>(match_payload.size())))
                Fail("failed to send GAME_MATCH");
        }

        void HandleGameMatch(const std::vector<std::uint8_t>& payload)
        {
            if (m_role != Role::Host || payload.size() != 4)
            {
                Fail("invalid GAME_MATCH");
                return;
            }

            const bool matched = ReadU32(payload.data()) != 0;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_peer_game_match = matched;
            }
            Log("GAME_MATCH received from client: %s", matched ? "OK" : "MISMATCH");
        }

        void HandlePrepareBoot()
        {
            if (m_role != Role::Client)
            {
                Fail("client-only PREPARE_BOOT received by host");
                return;
            }

            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!m_game_selected || !m_local_game_match || m_local_game_path.empty())
                {
                    m_last_error = "PREPARE_BOOT received but local game does not match host";
                    return;
                }
                ResetBootStateLocked();
                m_prepare_boot = true;
                m_boot_launch_pending = true;
            }
            Log("PREPARE_BOOT received; local matching game queued for synchronized launch");
        }

        void MaybeCommitStart()
        {
            if (m_role != Role::Host)
                return;

            bool commit = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (m_prepare_boot && m_local_boot_ready && m_peer_boot_ready && !m_start_committed)
                {
                    m_start_committed = true;
                    commit = true;
                }
            }

            if (!commit)
                return;

            Log("both VMs BOOT_READY; sending START_COMMIT");
            if (!SendControl(ControlType::StartCommit, nullptr, 0))
            {
                Fail("failed to send START_COMMIT");
                return;
            }
            m_boot_cv.notify_all();
        }

        void MaybeReleaseFirstPoll()
        {
            if (m_role != Role::Host)
                return;

            bool release = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (m_prepare_boot && m_start_committed && m_local_first_poll_ready &&
                    m_peer_first_poll_ready && !m_first_poll_go)
                {
                    m_first_poll_go = true;
                    release = true;
                }
            }

            if (!release)
                return;

            Log("both peers reached first DS2 poll; sending FIRST_POLL_GO");
            if (!SendControl(ControlType::FirstPollGo, nullptr, 0))
            {
                Fail("failed to send FIRST_POLL_GO");
                return;
            }
            m_first_poll_cv.notify_all();
        }

        bool WaitForRemoteFrame(std::uint32_t frame, InputFrame* output)
        {
            const auto started = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(m_remote_mutex);
            const bool ready = m_remote_cv.wait_for(lock, std::chrono::seconds(RECEIVE_TIMEOUT_SECONDS), [this, frame]() {
                return m_remote_frames.find(frame) != m_remote_frames.end() ||
                    !m_connected.load(std::memory_order_acquire) || m_stop_requested.load(std::memory_order_acquire);
            });
            const auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            const std::uint32_t wait = static_cast<std::uint32_t>(std::max<long long>(0, wait_ms));
            m_last_wait_ms.store(wait, std::memory_order_release);
            if (wait >= 1000)
                Log("STALL severe: poll=%u waited=%ums", static_cast<unsigned>(frame), static_cast<unsigned>(wait));
            else if (wait >= 250)
                Log("STALL: poll=%u waited=%ums", static_cast<unsigned>(frame), static_cast<unsigned>(wait));
            if (!ready)
                return false;

            const auto it = m_remote_frames.find(frame);
            if (it == m_remote_frames.end())
                return false;
            *output = it->second;
            return true;
        }

        void PruneFrames()
        {
            if (m_frame < 180)
                return;
            const std::uint32_t keep_from = m_frame - 120;
            for (auto it = m_local_frames.begin(); it != m_local_frames.end();)
                it = (it->first < keep_from) ? m_local_frames.erase(it) : std::next(it);
            std::lock_guard<std::mutex> lock(m_remote_mutex);
            for (auto it = m_remote_frames.begin(); it != m_remote_frames.end();)
                it = (it->first < keep_from) ? m_remote_frames.erase(it) : std::next(it);
        }

        void Fail(const char* message)
        {
            SetLastError(message);
            if (!m_failed.exchange(true, std::memory_order_acq_rel))
                Log("ERROR: %s (WSA=%d)", message, WSAGetLastError());
            m_connected.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            m_connecting.store(false, std::memory_order_release);
            m_remote_cv.notify_all();
            m_boot_cv.notify_all();
            m_first_poll_cv.notify_all();
        }

        Role m_role = Role::Disabled;
        std::string m_host = "127.0.0.1";
        std::uint16_t m_port = DEFAULT_PORT;
        std::atomic<std::uint32_t> m_delay{2};

        std::uint32_t m_frame = 0;
        bool m_have_capture = false;
        bool m_winsock_started = false;
        InputFrame m_capture = NEUTRAL_FRAME;
        InputFrame m_local_output = NEUTRAL_FRAME;
        InputFrame m_remote_output = NEUTRAL_FRAME;
        std::unordered_map<std::uint32_t, InputFrame> m_local_frames;
        std::unordered_map<std::uint32_t, InputFrame> m_remote_frames;

        SOCKET m_socket = INVALID_SOCKET;
        std::atomic<SOCKET> m_listener{INVALID_SOCKET};
        std::atomic<bool> m_connected{false};
        std::atomic<bool> m_connecting{false};
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_failed{false};
        std::atomic<bool> m_stop_requested{false};
        std::atomic<bool> m_connect_worker_started{false};
        std::atomic<std::uint32_t> m_last_wait_ms{0};

        std::mutex m_connect_mutex;
        std::mutex m_socket_mutex;
        std::mutex m_remote_mutex;
        std::condition_variable m_remote_cv;
        std::thread m_connector;
        std::thread m_receiver;

        mutable std::mutex m_state_mutex;
        std::condition_variable m_boot_cv;
        std::condition_variable m_first_poll_cv;
        std::string m_peer;
        std::string m_last_error;
        std::string m_log_path;

        bool m_game_selected = false;
        bool m_local_game_match = false;
        bool m_peer_game_match = false;
        std::string m_game_title;
        std::string m_game_serial;
        std::uint32_t m_game_crc = 0;
        std::string m_local_game_path;

        bool m_prepare_boot = false;
        bool m_boot_launch_pending = false;
        bool m_boot_launch_consumed = false;
        bool m_local_boot_ready = false;
        bool m_peer_boot_ready = false;
        bool m_start_committed = false;
        bool m_local_first_poll_ready = false;
        bool m_peer_first_poll_ready = false;
        bool m_first_poll_go = false;

        mutable std::mutex m_log_mutex;
        mutable FILE* m_log_file = nullptr;
        const std::chrono::steady_clock::time_point m_session_started = std::chrono::steady_clock::now();
    };

    Session& GetSession()
    {
        static Session session;
        return session;
    }
} // namespace

bool IsConfigured()
{
    return GetSession().IsConfigured();
}

void StartSessionAsync()
{
    GetSession().StartSessionAsync();
}

StatusSnapshot GetStatusSnapshot()
{
    return GetSession().GetStatusSnapshot();
}

bool HostSelectGame(const std::string& path, const std::string& title, const std::string& serial, std::uint32_t crc)
{
    return GetSession().HostSelectGame(path, title, serial, crc);
}

bool RequestSynchronizedBoot()
{
    return GetSession().RequestSynchronizedBoot();
}

bool ConsumeBootLaunchRequest(std::string* path)
{
    return GetSession().ConsumeBootLaunchRequest(path);
}

bool CanStartVM()
{
    return GetSession().CanStartVM();
}

bool ShouldHoldBootBarrier()
{
    return GetSession().ShouldHoldBootBarrier();
}

void NotifyBootReady()
{
    GetSession().NotifyBootReady();
}

bool WaitForStartCommit()
{
    return GetSession().WaitForStartCommit();
}

void ApplyDeterministicConfig()
{
    // v0.5a phase 1 focuses on synchronized selection/boot barriers. More
    // deterministic settings and memory-card shadowing are layered on top only
    // after this barrier is field-tested, to keep desync diagnosis attributable.
}

std::uint8_t HandlePadResponse(std::uint8_t unified_slot, std::uint32_t command_index, std::uint8_t local_value)
{
    return GetSession().HandlePadResponse(unified_slot, command_index, local_value);
}

void Shutdown()
{
    GetSession().Stop();
}
#else
bool IsConfigured() { return false; }
void StartSessionAsync() {}
StatusSnapshot GetStatusSnapshot() { return {}; }
bool HostSelectGame(const std::string&, const std::string&, const std::string&, std::uint32_t) { return false; }
bool RequestSynchronizedBoot() { return false; }
bool ConsumeBootLaunchRequest(std::string*) { return false; }
bool CanStartVM() { return true; }
bool ShouldHoldBootBarrier() { return false; }
void NotifyBootReady() {}
bool WaitForStartCommit() { return true; }
void ApplyDeterministicConfig() {}
std::uint8_t HandlePadResponse(std::uint8_t, std::uint32_t, std::uint8_t local_value) { return local_value; }
void Shutdown() {}
#endif
} // namespace ModernNetplay
