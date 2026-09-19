// SPDX-FileCopyrightText: 2026 PCSX2 Modern Netplay Port contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Netplay/ModernNetplay.h"

#include "Config.h"
#include "Counters.h"
#include "GameList.h"
#include "Host.h"
#include "INISettingsInterface.h"
#include "VMManager.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Memcard/MemoryCardFolder.h"
#include "SIO/Pad/Pad.h"
#include "DEV9/ACJV.h"

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
    static constexpr std::size_t ARCADE_JVS_STATE_BYTES = 40;
    using InputFrame = std::array<std::uint8_t, INPUT_FRAME_BYTES>;
    using InputBundle = std::array<InputFrame, MAX_PLAYERS>;

    struct ArcadePaths
    {
        std::string manifest;
        std::string boot;
        std::string media;
        std::string dongle_name;
        std::string dongle;
        std::string sram;
        std::string game_settings;
    };

    struct DeterminismFingerprint
    {
        std::uint64_t build = 0;
        std::uint64_t bios = 0;
        std::uint64_t gamedb = 0;
        std::uint64_t game_settings = 0;
        std::uint64_t media = 0;
        std::uint64_t arcade_manifest = 0;
        std::uint64_t boot = 0;
        std::uint64_t dongle = 0;
    };

    static constexpr std::uint32_t GAME_FLAG_ARCADE = 1u;
    static constexpr std::size_t GAME_MANIFEST_SIZE = 8 + 32 + 160 + (8 * 8);
    static constexpr std::uint32_t ARCADE_SESSION_MAGIC = 0x58364E50; // X6NP
    static constexpr std::uint32_t ARCADE_SESSION_VERSION = 1;
    static constexpr std::uint32_t ARCADE_SESSION_HEADER_SIZE = 16;

    constexpr std::uint32_t HELLO_MAGIC = 0x50324E50;   // P2NP
    constexpr std::uint32_t INPUT_MAGIC = 0x494E5054;   // INPT
    constexpr std::uint32_t BUNDLE_MAGIC = 0x424E444C;  // BNDL
    constexpr std::uint32_t CONTROL_MAGIC = 0x43544C31; // CTL1
    constexpr std::uint32_t PROTOCOL_VERSION = 11;
    constexpr std::uint16_t DEFAULT_PORT = 27886;
    constexpr int RECEIVE_TIMEOUT_SECONDS = 30;
    constexpr int BOOT_BARRIER_TIMEOUT_SECONDS = 90;
    constexpr DWORD SOCKET_SEND_TIMEOUT_MS = 8000;
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
        SyncCheckpoint = 18,
        MemcardAbort = 19,
        RoundEnd = 20,
        ArcadeJvsInput = 21,
        ArcadeJvsBundle = 22,
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

    void WriteU16(std::uint8_t* dst, std::uint16_t value)
    {
        value = htons(value);
        std::memcpy(dst, &value, sizeof(value));
    }

    std::uint16_t ReadU16(const std::uint8_t* src)
    {
        std::uint16_t value;
        std::memcpy(&value, src, sizeof(value));
        return ntohs(value);
    }

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

    void WriteArcadeJvsState(std::uint8_t* dst, const ArcadeJvsState& state)
    {
        WriteU32(dst + 0, state.mode);
        WriteU16(dst + 4, state.buttons);
        WriteU16(dst + 6, state.coin);
        WriteU16(dst + 8, state.screen_x);
        WriteU16(dst + 10, state.screen_y);
        WriteU16(dst + 12, state.raw_x);
        WriteU16(dst + 14, state.raw_y);
        for (std::size_t i = 0; i < state.drum.size(); i++)
            WriteU16(dst + 16 + i * 2, state.drum[i]);
        for (std::size_t i = 0; i < state.analog.size(); i++)
            WriteU16(dst + 24 + i * 2, state.analog[i]);
        WriteU16(dst + 30, state.dip_switch_state);
        WriteU16(dst + 32, state.test_state);
        WriteU16(dst + 34, state.flags);
        WriteU32(dst + 36, state.reserved);
    }

    ArcadeJvsState ReadArcadeJvsState(const std::uint8_t* src)
    {
        ArcadeJvsState state{};
        state.mode = ReadU32(src + 0);
        state.buttons = ReadU16(src + 4);
        state.coin = ReadU16(src + 6);
        state.screen_x = ReadU16(src + 8);
        state.screen_y = ReadU16(src + 10);
        state.raw_x = ReadU16(src + 12);
        state.raw_y = ReadU16(src + 14);
        for (std::size_t i = 0; i < state.drum.size(); i++)
            state.drum[i] = ReadU16(src + 16 + i * 2);
        for (std::size_t i = 0; i < state.analog.size(); i++)
            state.analog[i] = ReadU16(src + 24 + i * 2);
        state.dip_switch_state = ReadU16(src + 30);
        state.test_state = ReadU16(src + 32);
        state.flags = ReadU16(src + 34);
        state.reserved = ReadU32(src + 36);
        return state;
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

    bool HashFile64(const std::string& path, std::uint64_t* out_hash)
    {
        if (!out_hash || path.empty())
            return false;

        auto file = FileSystem::OpenManagedCFile(path.c_str(), "rb");
        if (!file)
            return false;

        std::uint64_t hash = 1469598103934665603ull;
        std::array<std::uint8_t, 1024 * 1024> buffer{};
        for (;;)
        {
            const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), file.get());
            for (std::size_t i = 0; i < count; i++)
            {
                hash ^= buffer[i];
                hash *= 1099511628211ull;
            }
            if (count < buffer.size())
            {
                if (std::ferror(file.get()))
                    return false;
                break;
            }
        }

        *out_hash = hash;
        return true;
    }

    std::uint64_t HashOptionalFile64(const std::string& path)
    {
        std::uint64_t hash = 0;
        if (!path.empty() && FileSystem::FileExists(path.c_str()))
            HashFile64(path, &hash);
        return hash;
    }

    std::uint64_t MixHash64(std::uint64_t seed, std::uint64_t value)
    {
        return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    }

    std::string ResolveGameSettingsPath(const std::string& serial, std::uint32_t crc, bool arcade)
    {
        if (arcade)
        {
            const std::string path = VMManager::GetGameSettingsPath(serial, 0);
            return FileSystem::FileExists(path.c_str()) ? path : std::string();
        }

        std::string path = VMManager::GetGameSettingsPath(serial, crc);
        if (FileSystem::FileExists(path.c_str()))
            return path;
        if (!serial.empty())
        {
            path = VMManager::GetGameSettingsPath(serial, 0);
            if (FileSystem::FileExists(path.c_str()))
                return path;
        }
        path = VMManager::GetGameSettingsPath({}, crc);
        return FileSystem::FileExists(path.c_str()) ? path : std::string();
    }

    bool ResolveArcadePaths(const std::string& manifest_path, const std::string& expected_serial,
        ArcadePaths* out, std::string* error)
    {
        if (!out || manifest_path.empty())
            return false;

        INISettingsInterface ini(manifest_path);
        if (!ini.Load())
        {
            if (error) *error = "无法读取街机 .acgame 配置";
            return false;
        }

        const std::string serial = ini.GetStringValue("game", "gameid");
        if (serial.empty() || (!expected_serial.empty() && !StringUtil::compareNoCase(serial, expected_serial)))
        {
            if (error) *error = "街机 .acgame 的 gameid 与房间游戏不一致";
            return false;
        }

        std::string base = Path::GetDirectory(manifest_path);
        const std::string subdir = ini.GetStringValue("data", "subdir", serial.c_str());
        if (!subdir.empty())
            base = Path::Combine(base, subdir);

        ArcadePaths paths{};
        paths.manifest = manifest_path;
        paths.boot = Path::Combine(base, ini.GetStringValue("data", "elf", "boot.elf"));
        paths.media = Path::Combine(base, ini.GetStringValue("data", "mediasrc", fmt::format("{}.chd", serial).c_str()));
        paths.dongle_name = ini.GetStringValue("data", "dongle", fmt::format("{}.ps2", serial).c_str());
        paths.dongle = Path::Combine(EmuFolders::MemoryCards, paths.dongle_name);
        paths.sram = Path::Combine(base, ini.GetStringValue("data", "sram", "sram.bin"));
        paths.game_settings = ResolveGameSettingsPath(serial, 0, true);
        *out = std::move(paths);
        return true;
    }

    bool BuildDeterminismFingerprint(const std::string& selected_path, const std::string& serial,
        std::uint32_t crc, bool arcade, DeterminismFingerprint* out, std::string* error)
    {
        if (!out)
            return false;

        DeterminismFingerprint fp{};
        const std::string upstream_sha = Path::Combine(EmuFolders::AppRoot, "PCSX2X6_UPSTREAM_SHA.txt");
        const std::string control_sha = Path::Combine(EmuFolders::AppRoot, "NETPLAY_CONTROL_SHA.txt");
        const std::uint64_t upstream_hash = HashOptionalFile64(upstream_sha);
        const std::uint64_t control_hash = HashOptionalFile64(control_sha);
        fp.build = MixHash64(upstream_hash, control_hash);
        if (upstream_hash == 0 || control_hash == 0)
        {
            if (error) *error = "缺少 PCSX2X6 联机版本标识文件，请重新下载完整联机包";
            return false;
        }

        if (!HashFile64(EmuConfig.FullpathToBios(), &fp.bios))
        {
            if (error) *error = "无法读取当前 BIOS，无法校验街机联机环境";
            return false;
        }

        const std::string gamedb_path = Path::Combine(EmuFolders::Resources, "GameIndex.yaml");
        if (!HashFile64(gamedb_path, &fp.gamedb))
        {
            if (error) *error = "无法读取 resources/GameIndex.yaml";
            return false;
        }

        if (!arcade)
        {
            fp.game_settings = HashOptionalFile64(ResolveGameSettingsPath(serial, crc, false));
            if (!HashFile64(selected_path, &fp.media))
            {
                if (error) *error = "无法读取所选 PS2 游戏镜像进行一致性校验";
                return false;
            }
            *out = fp;
            return true;
        }

        ArcadePaths paths{};
        if (!ResolveArcadePaths(selected_path, serial, &paths, error))
            return false;
        if (!HashFile64(paths.manifest, &fp.arcade_manifest))
        {
            if (error) *error = "无法读取街机 .acgame";
            return false;
        }
        if (!HashFile64(paths.boot, &fp.boot))
        {
            if (error) *error = "无法读取街机 boot.elf";
            return false;
        }
        if (!HashFile64(paths.media, &fp.media))
        {
            if (error) *error = "无法读取街机 CHD/DVD/HDD 镜像";
            return false;
        }
        if (!HashFile64(paths.dongle, &fp.dongle))
        {
            if (error) *error = "无法读取街机 Dongle：" + paths.dongle_name;
            return false;
        }
        fp.game_settings = HashOptionalFile64(paths.game_settings);
        *out = fp;
        return true;
    }

    std::string DescribeFingerprintMismatch(const DeterminismFingerprint& host,
        const DeterminismFingerprint& local, bool arcade)
    {
        std::string reason;
        const auto add = [&reason](const char* name) {
            if (!reason.empty()) reason += "、";
            reason += name;
        };
        if (host.build != local.build) add("联机/PCSX2X6版本");
        if (host.bios != local.bios) add("BIOS");
        if (host.gamedb != local.gamedb) add("GameIndex.yaml");
        if (host.game_settings != local.game_settings) add("游戏专用INI");
        if (host.media != local.media) add(arcade ? "街机游戏媒体" : "游戏镜像");
        if (arcade)
        {
            if (host.arcade_manifest != local.arcade_manifest) add(".acgame");
            if (host.boot != local.boot) add("boot.elf");
            if (host.dongle != local.dongle) add("Dongle");
        }
        return reason;
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

    void ConfigureConnectedSocket(SOCKET socket)
    {
        BOOL no_delay = TRUE;
        setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));

        const DWORD send_timeout = SOCKET_SEND_TIMEOUT_MS;
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&send_timeout), sizeof(send_timeout));
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
                RoleName(), m_username.c_str(), static_cast<unsigned>(m_port), static_cast<unsigned>(m_room_capacity),
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
                m_connecting.store(m_room_capacity > 1, std::memory_order_release);
                if (m_room_capacity > 1)
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
            out.max_players = m_room_capacity;
            out.round_players = m_max_players;
            out.local_player_id = m_local_player_id;
            out.session_id = m_session_id;
            out.last_error = m_last_error;
            out.log_path = m_log_path;
            out.player_count = RoomConnectedCountLocked();
            out.room_full = (out.player_count == m_room_capacity);
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
            out.memory_card_transfer_active = m_memory_card_transfer_active;
            out.memory_card_failed = m_memory_card_failed;
            out.memory_card_crc = m_memory_card_crc;
            out.memory_card_size = m_memory_card_size;
            out.memory_card_transferred_bytes =
                (m_role == Role::Host) ? m_memory_card_transferred_bytes : m_memcard_received_bytes;
            out.memory_card_status = m_memory_card_status;

            out.start_requested = m_start_requested;
            out.prepare_boot = m_prepare_boot;
            out.local_boot_ready = m_local_boot_ready;
            out.all_boot_ready = AllBootReadyLocked();
            out.start_committed = m_start_committed;
            out.first_poll_released = m_first_poll_go;
            out.round_in_progress = m_round_in_progress;
            out.game_switching = m_game_switching;
            out.topology_mode = m_topology_mode.load(std::memory_order_acquire);
            out.runtime_reconfiguring = m_runtime_reconfiguring;
            out.input_epoch = m_input_epoch.load(std::memory_order_acquire);
            return out;
        }

        bool HostSelectGame(const std::string& path, const std::string& title,
            const std::string& serial, std::uint32_t crc)
        {
            const bool arcade = VMManager::isArcadeManifest(path.c_str());
            if (m_role != Role::Host || path.empty() || serial.empty() ||
                (!arcade && crc == 0) || VMManager::HasValidVM())
                return false;
            if (arcade && m_room_capacity > 2)
            {
                SetLastError("System 246/256 JVS 街机联机当前支持 1～2 人，请先把房间人数调到 2 人以内");
                return false;
            }

            DeterminismFingerprint fingerprint{};
            std::string fingerprint_error;
            if (!BuildDeterminismFingerprint(path, serial, crc, arcade, &fingerprint, &fingerprint_error))
            {
                SetLastError(fingerprint_error.c_str());
                Log("determinism fingerprint failed: %s", fingerprint_error.c_str());
                return false;
            }

            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (m_prepare_boot || RoomConnectedCountLocked() != m_room_capacity)
                    return false;

                m_max_players = m_room_capacity;
                m_round_in_progress = false;
                m_game_switching = false;
                m_game_selected = true;
                m_local_game_match = true;
                m_game_title = title;
                m_game_serial = serial;
                m_game_crc = crc;
                m_local_game_path = path;
                m_game_is_arcade = arcade;
                m_game_fingerprint = fingerprint;
                m_last_error.clear();
                ResetStartStateLocked();
                for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
                    m_players[i].game_match = (i == 0 && i < m_max_players);
            }

            Log("host selected game: title=%s serial=%s crc=%08X arcade=%s build=%016llX bios=%016llX gamedb=%016llX gameini=%016llX media=%016llX acgame=%016llX boot=%016llX dongle=%016llX",
                title.c_str(), serial.c_str(), crc, arcade ? "yes" : "no",
                static_cast<unsigned long long>(fingerprint.build),
                static_cast<unsigned long long>(fingerprint.bios),
                static_cast<unsigned long long>(fingerprint.gamedb),
                static_cast<unsigned long long>(fingerprint.game_settings),
                static_cast<unsigned long long>(fingerprint.media),
                static_cast<unsigned long long>(fingerprint.arcade_manifest),
                static_cast<unsigned long long>(fingerprint.boot),
                static_cast<unsigned long long>(fingerprint.dongle));
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
                if (RoundConnectedCountLocked() != m_max_players || m_prepare_boot)
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

            // Normalize state-affecting user settings before PCSX2 applies the
            // shared game-database overrides. This removes per-machine timing
            // differences while still allowing identical game fixes on all peers.
            EmuConfig.Cpu = Pcsx2Config::CpuOptions();
            EmuConfig.Speedhacks.DisableAll();
            EmuConfig.EmulationSpeed.NominalScalar = 1.0f;
            EmuConfig.EmulationSpeed.SyncToHostRefreshRate = false;
            EmuConfig.EmulationSpeed.UseVSyncForTiming = false;
            EmuConfig.GS.FramerateNTSC = Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_NTSC;
            EmuConfig.GS.FrameratePAL = Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_PAL;
            EmuConfig.EnablePatches = true;
            EmuConfig.EnableGameFixes = true;
            EmuConfig.EnableCheats = false;
            EmuConfig.EnableWideScreenPatches = false;
            EmuConfig.EnableNoInterlacingPatches = false;
            EmuConfig.EnableFastBoot = true;

            // The PS2 RTC is visible to games. Using each PC's wall clock can make
            // otherwise identical VMs take different branches, so Netplay pins it.
            EmuConfig.ManuallySetRealTimeClock = true;
            EmuConfig.RtcYear = 0;   // PCSX2 interprets this as year 2000.
            EmuConfig.RtcMonth = 1;
            EmuConfig.RtcDay = 1;
            EmuConfig.RtcHour = 0;
            EmuConfig.RtcMinute = 0;
            EmuConfig.RtcSecond = 0;

            Log("deterministic core normalized: CPU=default speedhacks=off nominal=100%% "
                "NTSC=%.2f PAL=%.2f gamedb=on gamefixes=on local-cheats=off local-patches=off rtc=2000-01-01",
                static_cast<double>(EmuConfig.GS.FramerateNTSC),
                static_cast<double>(EmuConfig.GS.FrameratePAL));

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
            // System 246/256 games read their controls from JVS/ACJV instead of
            // the DualShock2 poll path. Do not advance the shared Netplay poll twice.
            if (ACJV::enabled)
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
            return RequestRuntimeSettings(local_controller, delay, topology);
        }

        bool RequestRoomCapacityImpl(std::uint32_t capacity)
        {
            return RequestRoomCapacity(capacity);
        }

        bool RequestReturnToLobbyImpl()
        {
            return RequestReturnToLobby();
        }

        bool SynchronizeArcadeJvsImpl(const ArcadeJvsState& local_state, ArcadeJvsBundle* bundle)
        {
            return SynchronizeArcadeJvsInternal(local_state, bundle);
        }

        std::string GetArcadeDongleOverrideImpl(const std::string& original_filename) const
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            return (m_game_is_arcade && m_memory_card_local_ready && !m_shadow_card_filename.empty()) ?
                m_shadow_card_filename : original_filename;
        }

        std::string GetArcadeSramOverrideImpl(const std::string& original_path) const
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            return (m_game_is_arcade && m_memory_card_local_ready && !m_shadow_arcade_sram_path.empty()) ?
                m_shadow_arcade_sram_path : original_path;
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
            m_room_capacity = m_max_players;
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

        std::uint32_t RoomConnectedCountLocked() const
        {
            std::uint32_t count = 0;
            for (std::uint32_t i = 0; i < m_room_capacity; i++)
                count += m_players[i].connected ? 1u : 0u;
            return count;
        }

        std::uint32_t RoundConnectedCountLocked() const
        {
            std::uint32_t count = 0;
            for (std::uint32_t i = 0; i < m_max_players; i++)
                count += m_players[i].connected ? 1u : 0u;
            return count;
        }

        bool AllGamesMatchLocked() const
        {
            if (!m_game_selected || RoundConnectedCountLocked() != m_max_players)
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
            if (RoundConnectedCountLocked() != m_max_players)
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
            if (!m_prepare_boot || RoundConnectedCountLocked() != m_max_players)
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
            if (!m_prepare_boot || RoundConnectedCountLocked() != m_max_players)
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
            m_memory_card_transfer_active = false;
            m_memory_card_failed = false;
            m_memory_card_crc = 0;
            m_memory_card_size = 0;
            m_memory_card_transferred_bytes = 0;
            m_memcard_received_bytes = 0;
            m_memcard_receive.clear();
            m_memory_card_status = m_memory_card_sync_enabled ? "等待记忆卡同步" : "记忆卡同步已关闭";
            m_shadow_card_filename.clear();
            m_shadow_arcade_sram_path.clear();
            m_prepare_boot = false;
            m_boot_launch_pending = false;
            m_boot_launch_consumed = false;
            m_local_boot_ready = false;
            m_start_committed = false;
            m_first_poll_go = false;
            m_frame = 0;
            m_frame_counter.store(0, std::memory_order_release);
            m_runtime_reconfiguring = false;
            m_runtime_commit_pending = false;
            m_have_capture = false;
            m_capture = NEUTRAL_FRAME;
            m_output_bundle.fill(NEUTRAL_FRAME);
            m_host_sync_checkpoints.clear();
            m_desync_strikes.fill(0);
            for (auto& player : m_players)
            {
                player.memcard_ready = false;
                player.boot_ready = false;
                player.first_poll_ready = false;
            }
            for (auto& map : m_player_inputs)
                map.clear();
            m_bundles.clear();
            for (auto& map : m_arcade_jvs_inputs)
                map.clear();
            m_arcade_jvs_bundles.clear();
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
            std::uint32_t round_info = 0;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                round_info = (m_max_players & 0xffu) | (m_round_in_progress ? 0x80000000u : 0u);
            }
            WriteU32(packet->data() + 36, round_info);
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
            for (std::uint32_t id = 2; id <= m_room_capacity; id++)
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
            std::uint32_t capacity = 0;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (m_runtime_reconfiguring)
                    return false;
                assigned = FindFreePlayerIdLocked();
                capacity = m_room_capacity;
            }
            if (assigned == 0)
                return false;

            std::string client_name = ReadHelloName(incoming);
            if (client_name.empty())
                client_name = "玩家" + std::to_string(assigned);

            std::array<std::uint8_t, HELLO_SIZE> outgoing{};
            BuildHello(&outgoing, Role::Host, capacity, assigned, m_session_id,
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
            const std::uint32_t round_info = ReadU32(incoming.data() + 36);
            const std::uint32_t round_players = round_info & 0xffu;
            const bool round_in_progress = (round_info & 0x80000000u) != 0;
            if (max_players < 1 || max_players > MAX_PLAYERS || assigned < 2 || assigned > max_players ||
                round_players < 1 || round_players > max_players)
                return false;

            const std::string host_name = ReadHelloName(incoming);
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_room_capacity = max_players;
                m_max_players = round_players;
                m_round_in_progress = round_in_progress;
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

            Log("room open: TCP port %u, capacity=%u", static_cast<unsigned>(m_port), static_cast<unsigned>(m_room_capacity));
            while (!m_stop_requested.load(std::memory_order_acquire))
            {
                {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    m_connecting.store(RoomConnectedCountLocked() < m_room_capacity, std::memory_order_release);
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

                ConfigureConnectedSocket(socket_value);

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
                bool round_active = false;
                {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    selected = m_game_selected;
                    round_active = m_round_in_progress || m_prepare_boot || m_start_committed;
                }
                if (selected && !round_active)
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
                        ConfigureConnectedSocket(connected);
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

        bool BroadcastControlChecked(ControlType type, const void* payload, std::uint32_t size,
            std::uint32_t* failed_player = nullptr, std::uint32_t skip_player = 0)
        {
            bool ok = true;
            if (failed_player)
                *failed_player = 0;

            std::lock_guard<std::mutex> lock(m_peer_mutex);
            for (auto& peer : m_peers)
            {
                if (!peer || !peer->connected.load(std::memory_order_acquire) || peer->player_id == skip_player)
                    continue;

                if (!SendControlToPeer(*peer, type, payload, size))
                {
                    Log("warning: failed control send to P%u type=%u winsock=%d",
                        static_cast<unsigned>(peer->player_id),
                        static_cast<unsigned>(type), WSAGetLastError());
                    if (failed_player && *failed_player == 0)
                        *failed_player = peer->player_id;
                    ok = false;
                }
            }
            return ok;
        }

        void BroadcastControl(ControlType type, const void* payload, std::uint32_t size)
        {
            BroadcastControlChecked(type, payload, size);
        }

        void BroadcastMemoryCardAbort(const std::string& reason, std::uint32_t skip_player = 0)
        {
            const std::size_t size = std::min<std::size_t>(reason.size(), MAX_CONTROL_PAYLOAD);
            BroadcastControlChecked(ControlType::MemcardAbort,
                size > 0 ? reinterpret_cast<const std::uint8_t*>(reason.data()) : nullptr,
                static_cast<std::uint32_t>(size), nullptr, skip_player);
        }

        void BroadcastRoster()
        {
            if (m_role != Role::Host)
                return;
            std::array<std::uint8_t, 12 + MAX_PLAYERS * 48> payload{};
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                WriteU32(payload.data() + 0, m_room_capacity);
                WriteU32(payload.data() + 4, m_max_players);
                std::uint32_t room_flags = 0;
                room_flags |= m_round_in_progress ? 1u : 0u;
                room_flags |= m_game_switching ? 2u : 0u;
                WriteU32(payload.data() + 8, room_flags);
                for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
                {
                    std::uint8_t* row = payload.data() + 12 + i * 48;
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

        bool BuildGameManifest(std::array<std::uint8_t, GAME_MANIFEST_SIZE>* payload) const
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            if (!m_game_selected)
                return false;
            payload->fill(0);
            WriteU32(payload->data() + 0, m_game_crc);
            WriteU32(payload->data() + 4, m_game_is_arcade ? GAME_FLAG_ARCADE : 0u);
            std::snprintf(reinterpret_cast<char*>(payload->data() + 8), 32, "%s", m_game_serial.c_str());
            std::snprintf(reinterpret_cast<char*>(payload->data() + 40), 160, "%s", m_game_title.c_str());
            WriteU64(payload->data() + 200, m_game_fingerprint.build);
            WriteU64(payload->data() + 208, m_game_fingerprint.bios);
            WriteU64(payload->data() + 216, m_game_fingerprint.gamedb);
            WriteU64(payload->data() + 224, m_game_fingerprint.game_settings);
            WriteU64(payload->data() + 232, m_game_fingerprint.media);
            WriteU64(payload->data() + 240, m_game_fingerprint.arcade_manifest);
            WriteU64(payload->data() + 248, m_game_fingerprint.boot);
            WriteU64(payload->data() + 256, m_game_fingerprint.dongle);
            return true;
        }

        bool BroadcastGameManifest()
        {
            if (m_max_players == 1)
                return true;
            std::array<std::uint8_t, GAME_MANIFEST_SIZE> payload{};
            if (!BuildGameManifest(&payload))
                return false;
            BroadcastControl(ControlType::GameManifest, payload.data(), static_cast<std::uint32_t>(payload.size()));
            return true;
        }

        bool SendGameManifestToPeer(Peer& peer)
        {
            std::array<std::uint8_t, GAME_MANIFEST_SIZE> payload{};
            if (!BuildGameManifest(&payload))
                return false;
            return SendControlToPeer(peer, ControlType::GameManifest, payload.data(), static_cast<std::uint32_t>(payload.size()));
        }

        void HandleRoster(const std::vector<std::uint8_t>& payload)
        {
            if (payload.size() != (12 + MAX_PLAYERS * 48))
                return;
            const std::uint32_t room_capacity = ReadU32(payload.data() + 0);
            const std::uint32_t round_players = ReadU32(payload.data() + 4);
            const std::uint32_t room_flags = ReadU32(payload.data() + 8);
            if (room_capacity < 1 || room_capacity > MAX_PLAYERS ||
                round_players < 1 || round_players > room_capacity)
                return;
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_room_capacity = room_capacity;
            m_max_players = round_players;
            m_round_in_progress = (room_flags & 1u) != 0;
            m_game_switching = (room_flags & 2u) != 0;
            for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
            {
                const std::uint8_t* row = payload.data() + 12 + i * 48;
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
            if (m_role != Role::Client || payload.size() != GAME_MANIFEST_SIZE)
                return;

            const std::uint32_t crc = ReadU32(payload.data() + 0);
            const bool arcade = (ReadU32(payload.data() + 4) & GAME_FLAG_ARCADE) != 0;
            const char* serial_ptr = reinterpret_cast<const char*>(payload.data() + 8);
            const char* title_ptr = reinterpret_cast<const char*>(payload.data() + 40);
            std::size_t serial_len = 0;
            while (serial_len < 32 && serial_ptr[serial_len] != '\0')
                ++serial_len;
            std::size_t title_len = 0;
            while (title_len < 160 && title_ptr[title_len] != '\0')
                ++title_len;
            const std::string serial(serial_ptr, serial_len);
            const std::string title(title_ptr, title_len);

            DeterminismFingerprint host_fp{};
            host_fp.build = ReadU64(payload.data() + 200);
            host_fp.bios = ReadU64(payload.data() + 208);
            host_fp.gamedb = ReadU64(payload.data() + 216);
            host_fp.game_settings = ReadU64(payload.data() + 224);
            host_fp.media = ReadU64(payload.data() + 232);
            host_fp.arcade_manifest = ReadU64(payload.data() + 240);
            host_fp.boot = ReadU64(payload.data() + 248);
            host_fp.dongle = ReadU64(payload.data() + 256);

            std::string local_path;
            {
                auto lock = GameList::GetLock();
                const GameList::Entry* entry = GameList::GetEntryBySerialAndCRC(serial, crc);
                if (entry)
                    local_path = entry->path;
            }

            const bool local_arcade = !local_path.empty() && VMManager::isArcadeManifest(local_path.c_str());
            DeterminismFingerprint local_fp{};
            std::string fingerprint_error;
            bool matched = !local_path.empty() && (local_arcade == arcade) &&
                BuildDeterminismFingerprint(local_path, serial, crc, arcade, &local_fp, &fingerprint_error);
            std::string mismatch;
            if (matched)
            {
                mismatch = DescribeFingerprintMismatch(host_fp, local_fp, arcade);
                matched = mismatch.empty();
            }
            else if (fingerprint_error.empty())
            {
                fingerprint_error = local_path.empty() ?
                    "本机没有找到相同的游戏" : "本机游戏类型与房主不一致";
            }

            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_game_selected = true;
                m_game_title = title;
                m_game_serial = serial;
                m_game_crc = crc;
                m_local_game_path = local_path;
                m_game_is_arcade = arcade;
                m_game_fingerprint = host_fp;
                m_local_game_match = matched;
                ResetStartStateLocked();
                m_game_is_arcade = arcade;
                m_game_fingerprint = host_fp;
                m_last_error = matched ? std::string() :
                    ("联机启动环境不一致：" + (!mismatch.empty() ? mismatch : fingerprint_error));
                if (m_local_player_id >= 1 && m_local_player_id <= MAX_PLAYERS)
                    m_players[m_local_player_id - 1].game_match = matched;
                m_players[0].game_match = true;
            }

            std::array<std::uint8_t, 8> reply{};
            WriteU32(reply.data(), m_local_player_id);
            WriteU32(reply.data() + 4, matched ? 1u : 0u);
            SendControlToHost(ControlType::GameMatch, reply.data(), static_cast<std::uint32_t>(reply.size()));
            Log("GAME_MANIFEST: title=%s serial=%s crc=%08X arcade=%s local_match=%s mismatch=%s",
                title.c_str(), serial.c_str(), crc, arcade ? "yes" : "no",
                matched ? "yes" : "no",
                mismatch.empty() ? fingerprint_error.c_str() : mismatch.c_str());
        }

        bool ExportArcadeSessionState(std::vector<std::uint8_t>* data, bool* present, std::string* error)
        {
            std::string path;
            std::string serial;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                path = m_local_game_path;
                serial = m_game_serial;
            }

            ArcadePaths paths{};
            if (!ResolveArcadePaths(path, serial, &paths, error))
                return false;

            const std::optional<std::vector<u8>> dongle = FileSystem::ReadBinaryFile(paths.dongle.c_str());
            if (!dongle.has_value() || dongle->empty())
            {
                if (error) *error = "无法读取房主街机 Dongle：" + paths.dongle_name;
                return false;
            }

            std::vector<u8> sram;
            if (FileSystem::FileExists(paths.sram.c_str()))
            {
                const std::optional<std::vector<u8>> read_sram = FileSystem::ReadBinaryFile(paths.sram.c_str());
                if (!read_sram.has_value())
                {
                    if (error) *error = "无法读取房主街机 sram.bin";
                    return false;
                }
                sram = *read_sram;
            }

            if (dongle->size() > (32u * 1024u * 1024u) || sram.size() > (32u * 1024u * 1024u))
            {
                if (error) *error = "街机 Dongle 或 SRAM 文件大小异常";
                return false;
            }

            const std::size_t total = ARCADE_SESSION_HEADER_SIZE + dongle->size() + sram.size();
            if (total > (80u * 1024u * 1024u))
            {
                if (error) *error = "街机联机状态包过大";
                return false;
            }

            data->assign(total, 0);
            WriteU32(data->data() + 0, ARCADE_SESSION_MAGIC);
            WriteU32(data->data() + 4, ARCADE_SESSION_VERSION);
            WriteU32(data->data() + 8, static_cast<std::uint32_t>(dongle->size()));
            WriteU32(data->data() + 12, static_cast<std::uint32_t>(sram.size()));
            std::memcpy(data->data() + ARCADE_SESSION_HEADER_SIZE, dongle->data(), dongle->size());
            if (!sram.empty())
                std::memcpy(data->data() + ARCADE_SESSION_HEADER_SIZE + dongle->size(), sram.data(), sram.size());

            *present = true;
            Log("arcade session state prepared: dongle=%s bytes=%u sram=%s bytes=%u",
                paths.dongle_name.c_str(), static_cast<unsigned>(dongle->size()),
                sram.empty() ? "blank" : paths.sram.c_str(), static_cast<unsigned>(sram.size()));
            return true;
        }

        bool ExportConfiguredMemoryCard(std::vector<std::uint8_t>* data, bool* present, std::string* error)
        {
            data->clear();
            *present = false;

            bool arcade = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                arcade = m_game_is_arcade;
            }
            if (arcade)
                return ExportArcadeSessionState(data, present, error);

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
            bool arcade = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                arcade = m_game_is_arcade;
            }

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

            if (arcade)
            {
                if (data.size() < ARCADE_SESSION_HEADER_SIZE ||
                    ReadU32(data.data() + 0) != ARCADE_SESSION_MAGIC ||
                    ReadU32(data.data() + 4) != ARCADE_SESSION_VERSION)
                {
                    Log("invalid PCSX2X6 arcade session-state package");
                    return false;
                }

                const std::uint32_t dongle_size = ReadU32(data.data() + 8);
                const std::uint32_t sram_size = ReadU32(data.data() + 12);
                const std::size_t expected = ARCADE_SESSION_HEADER_SIZE +
                    static_cast<std::size_t>(dongle_size) + static_cast<std::size_t>(sram_size);
                if (expected != data.size() || dongle_size == 0)
                {
                    Log("invalid arcade session-state sizes");
                    return false;
                }

                char dongle_name[96]{};
                std::snprintf(dongle_name, sizeof(dongle_name), "NetplayX6Dongle-%016llX.ps2",
                    static_cast<unsigned long long>(m_session_id));
                const std::string dongle_path = Path::Combine(EmuFolders::MemoryCards, dongle_name);
                if (!FileSystem::WriteBinaryFile(dongle_path.c_str(),
                        data.data() + ARCADE_SESSION_HEADER_SIZE, dongle_size))
                {
                    Log("failed to write Netplay X6 shadow dongle: %s", dongle_path.c_str());
                    return false;
                }

                const std::string state_dir = Path::Combine(EmuFolders::DataRoot, "netplay");
                if (!FileSystem::DirectoryExists(state_dir.c_str()) &&
                    !FileSystem::CreateDirectoryPath(state_dir.c_str(), true))
                {
                    FileSystem::DeleteFilePath(dongle_path.c_str());
                    Log("failed to create Netplay X6 state directory: %s", state_dir.c_str());
                    return false;
                }

                char sram_name[96]{};
                std::snprintf(sram_name, sizeof(sram_name), "NetplayX6SRAM-%016llX.bin",
                    static_cast<unsigned long long>(m_session_id));
                const std::string sram_path = Path::Combine(state_dir, sram_name);
                FileSystem::DeleteFilePath(sram_path.c_str());
                if (sram_size > 0 && !FileSystem::WriteBinaryFile(sram_path.c_str(),
                        data.data() + ARCADE_SESSION_HEADER_SIZE + dongle_size, sram_size))
                {
                    FileSystem::DeleteFilePath(dongle_path.c_str());
                    Log("failed to write Netplay X6 shadow SRAM: %s", sram_path.c_str());
                    return false;
                }

                {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    m_shadow_arcade_sram_path = sram_path;
                }
                *out_filename = dongle_name;
                Log("Netplay X6 shadows written: dongle=%s sram=%s sram_bytes=%u",
                    dongle_path.c_str(), sram_path.c_str(), static_cast<unsigned>(sram_size));
                return true;
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

        void CancelPendingStart(const std::string& message, std::uint32_t skip_notify_player = 0)
        {
            std::uint32_t total = 0;
            std::uint64_t transferred = 0;
            bool present = false;
            std::string shadow_to_remove;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                total = m_memory_card_size;
                transferred = m_memory_card_transferred_bytes;
                present = m_memory_card_present;
                shadow_to_remove = m_shadow_card_filename;
                ResetStartStateLocked();
                m_memory_card_present = present;
                m_memory_card_size = total;
                m_memory_card_transferred_bytes = transferred;
                m_memory_card_transfer_active = false;
                m_memory_card_failed = true;
                m_last_error = message;
                m_memory_card_status = message;
            }

            if (!shadow_to_remove.empty())
            {
                const std::string shadow_path = Path::Combine(EmuFolders::MemoryCards, shadow_to_remove);
                FileSystem::DeleteFilePath(shadow_path.c_str());
            }

            Log("START_CANCELLED: %s", message.c_str());
            if (m_role == Role::Host)
                BroadcastMemoryCardAbort(message, skip_notify_player);
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
                m_memory_card_transferred_bytes = 0;
                m_memory_card_transfer_active = true;
                m_memory_card_failed = false;
                m_shadow_card_filename = shadow;
                m_memory_card_local_ready = true;
                m_players[0].memcard_ready = true;
                m_memory_card_status = m_game_is_arcade ?
                    "正在同步街机 Dongle + SRAM 临时状态" :
                    (present ? "正在发送房主记忆卡临时副本" : "房主未插入记忆卡，正在同步无卡状态");
            }

            std::array<std::uint8_t, 16> begin{};
            WriteU32(begin.data(), present ? 1u : 0u);
            WriteU32(begin.data() + 4, static_cast<std::uint32_t>(data.size()));
            WriteU64(begin.data() + 8, hash);

            std::uint32_t failed_player = 0;
            if (!BroadcastControlChecked(ControlType::MemcardBegin, begin.data(),
                    static_cast<std::uint32_t>(begin.size()), &failed_player))
            {
                CancelPendingStart("向玩家 P" + std::to_string(failed_player) +
                    " 发送记忆卡同步信息超时或失败，请检查连接后重试", failed_player);
                return;
            }

            std::uint32_t last_logged_percent = 0;
            for (std::uint32_t offset = 0; offset < data.size(); offset += MEMCARD_CHUNK)
            {
                const std::uint32_t chunk_size = std::min<std::uint32_t>(MEMCARD_CHUNK,
                    static_cast<std::uint32_t>(data.size()) - offset);
                std::vector<std::uint8_t> chunk(4u + chunk_size);
                WriteU32(chunk.data(), offset);
                std::memcpy(chunk.data() + 4, data.data() + offset, chunk_size);

                failed_player = 0;
                if (!BroadcastControlChecked(ControlType::MemcardChunk, chunk.data(),
                        static_cast<std::uint32_t>(chunk.size()), &failed_player))
                {
                    CancelPendingStart("向玩家 P" + std::to_string(failed_player) +
                        " 发送记忆卡数据超时或失败，本次启动已取消；请检查该玩家网络后重试",
                        failed_player);
                    return;
                }

                const std::uint64_t completed = static_cast<std::uint64_t>(offset) + chunk_size;
                {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    m_memory_card_transferred_bytes = completed;
                }

                if (!data.empty())
                {
                    const std::uint32_t percent =
                        static_cast<std::uint32_t>((completed * 100u) / data.size());
                    if (percent >= last_logged_percent + 10u || percent == 100u)
                    {
                        last_logged_percent = percent;
                        Log("memory card send progress: %u%% (%llu/%u bytes)",
                            static_cast<unsigned>(percent),
                            static_cast<unsigned long long>(completed),
                            static_cast<unsigned>(data.size()));
                    }
                }
            }

            failed_player = 0;
            if (!BroadcastControlChecked(ControlType::MemcardEnd, nullptr, 0, &failed_player))
            {
                CancelPendingStart("向玩家 P" + std::to_string(failed_player) +
                    " 发送记忆卡结束标记失败，本次启动已取消", failed_player);
                return;
            }

            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_memory_card_transferred_bytes = data.size();
                m_memory_card_status = m_game_is_arcade ?
                    "街机 Dongle + SRAM 已发送，等待其他玩家校验" :
                    (present ? "记忆卡发送完成，等待其他玩家校验" : "无卡状态已发送，等待其他玩家确认");
            }

            BroadcastRoster();
            Log("memory card shadow prepared: present=%s size=%u hash=%016llX",
                present ? "yes" : "no", static_cast<unsigned>(data.size()),
                static_cast<unsigned long long>(hash));
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
            m_memory_card_transfer_active = true;
            m_memory_card_failed = false;
            m_memory_card_local_ready = false;
            m_memory_card_status = m_game_is_arcade ?
                "正在接收房主街机 Dongle + SRAM 临时状态" :
                (present ? "正在接收房主记忆卡临时副本" : "正在接收房主无卡状态");
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
                m_memory_card_transfer_active = false;
                m_memory_card_failed = true;
                m_last_error = "记忆卡同步失败：收到的数据块越界";
                m_memory_card_status = m_last_error;
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
                m_memory_card_transfer_active = false;
                m_memory_card_failed = !ok;
                m_shadow_card_filename = ok ? shadow : std::string();
                if (m_local_player_id >= 1 && m_local_player_id <= MAX_PLAYERS)
                    m_players[m_local_player_id - 1].memcard_ready = ok;
                m_memory_card_status = ok ?
                    (m_game_is_arcade ? "街机 Dongle + SRAM 同步完成 ✓（使用联机临时副本）" :
                        (present ? "记忆卡同步完成 ✓（使用联机临时副本）" : "无记忆卡状态同步完成 ✓")) :
                    (m_game_is_arcade ? "街机 Dongle/SRAM 同步失败：大小或校验值不一致" :
                        "记忆卡同步失败：大小或校验值不一致");
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
                SetLastError("记忆卡同步失败：大小或校验值不一致，可在房间中重新选择游戏重试");
                Log("memory card synchronization failed; lobby remains open for retry");
            }
        }

        void HandleClientMemcardAbort(const std::vector<std::uint8_t>& payload)
        {
            const std::string reason = payload.empty() ? "房主取消了本次记忆卡同步" :
                std::string(reinterpret_cast<const char*>(payload.data()), payload.size());

            std::string shadow_to_remove;
            std::uint32_t total = 0;
            std::uint64_t received = 0;
            bool present = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                total = m_memory_card_size;
                received = m_memcard_received_bytes;
                present = m_memory_card_present;
                shadow_to_remove = m_shadow_card_filename;
                ResetStartStateLocked();
                m_memory_card_present = present;
                m_memory_card_size = total;
                m_memcard_received_bytes = received;
                m_memory_card_transfer_active = false;
                m_memory_card_failed = true;
                m_memory_card_status = reason;
                m_last_error = reason;
            }

            if (!shadow_to_remove.empty())
            {
                const std::string shadow_path = Path::Combine(EmuFolders::MemoryCards, shadow_to_remove);
                FileSystem::DeleteFilePath(shadow_path.c_str());
            }

            Log("MEMCARD_ABORT received: %s", reason.c_str());
        }

        void MaybeAdvanceStart()
        {
            if (m_role != Role::Host)
                return;

            bool start_memcard = false;
            bool begin_boot = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!m_start_requested || m_prepare_boot || RoundConnectedCountLocked() != m_max_players || !AllGamesMatchLocked())
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
                    m_round_in_progress = true;
                    m_game_switching = false;
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

        bool StageRuntimeConfigLocal(std::uint32_t change_id, std::uint32_t delay,
            std::uint32_t topology, const std::array<std::uint32_t, MAX_PLAYERS>& controllers)
        {
            if (change_id == 0 || delay < 1 || delay > 100 || topology > 1 ||
                !ValidateControllerMap(controllers))
                return false;
            const std::uint32_t old_topology = m_topology_mode.load(std::memory_order_acquire);
            if (VMManager::HasValidVM() && topology != old_topology)
                return false;

            std::lock_guard<std::mutex> lock(m_state_mutex);
            if (m_runtime_reconfiguring && m_runtime_change_id != change_id)
                return false;
            m_runtime_reconfiguring = true;
            m_runtime_change_id = change_id;
            m_runtime_commit_pending = false;
            m_pending_runtime_delay = delay;
            m_pending_runtime_topology = topology;
            m_pending_runtime_controllers = controllers;
            return true;
        }

        void ApplyCommittedRuntimeConfig(std::uint32_t change_id)
        {
            std::uint32_t delay = 0;
            std::uint32_t topology = 0;
            std::array<std::uint32_t, MAX_PLAYERS> controllers{};
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!m_runtime_reconfiguring || !m_runtime_commit_pending ||
                    m_runtime_change_id != change_id)
                    return;
                delay = m_pending_runtime_delay;
                topology = m_pending_runtime_topology;
                controllers = m_pending_runtime_controllers;
                for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
                    m_players[i].controller = controllers[i];
                m_runtime_reconfiguring = false;
                m_runtime_commit_pending = false;
                m_runtime_change_id = 0;
            }
            m_delay.store(delay, std::memory_order_release);
            m_topology_mode.store(topology, std::memory_order_release);
            Host::AddKeyedOSDMessage("ModernNetplayRuntimeConfig",
                "联机实时设置已在统一输入点生效。", 5.0f);
            Log("runtime config applied: change=%u frame=%u delay=%u",
                static_cast<unsigned>(change_id), static_cast<unsigned>(m_frame),
                static_cast<unsigned>(delay));
            if (m_role == Role::Host)
                BroadcastRoster();
        }

        void ScheduleRuntimeConfigCommit(std::uint32_t change_id, std::uint32_t apply_frame)
        {
            bool apply_now = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!m_runtime_reconfiguring || m_runtime_change_id != change_id)
                    return;
                m_runtime_commit_frame = apply_frame;
                m_runtime_commit_pending = true;
                apply_now = !VMManager::HasValidVM();
            }
            if (apply_now)
                ApplyCommittedRuntimeConfig(change_id);
        }

        void ApplyPendingRuntimeConfigIfDue()
        {
            std::uint32_t change_id = 0;
            std::uint32_t target = 0;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!m_runtime_reconfiguring || !m_runtime_commit_pending)
                    return;
                change_id = m_runtime_change_id;
                target = m_runtime_commit_frame;
            }
            if (m_frame >= target)
                ApplyCommittedRuntimeConfig(change_id);
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
                for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
                {
                    if (m_runtime_required_acks[i] && !m_runtime_acks[i])
                    {
                        ready = false;
                        break;
                    }
                }
                change_id = m_runtime_change_id;
            }
            if (!ready)
                return;

            const std::uint32_t apply_frame = VMManager::HasValidVM() ?
                (m_frame_counter.load(std::memory_order_acquire) + 120u) : 0u;
            std::array<std::uint8_t, 8> payload{};
            WriteU32(payload.data(), change_id);
            WriteU32(payload.data() + 4, apply_frame);
            BroadcastControl(ControlType::RuntimeCommit, payload.data(),
                static_cast<std::uint32_t>(payload.size()));
            ScheduleRuntimeConfigCommit(change_id, apply_frame);
            Log("runtime config target: change=%u poll=%u",
                static_cast<unsigned>(change_id), static_cast<unsigned>(apply_frame));
        }

        bool BeginHostRuntimeConfig(std::uint32_t delay, std::uint32_t topology,
            std::array<std::uint32_t, MAX_PLAYERS> controllers)
        {
            if (m_role != Role::Host || delay < 1 || delay > 100 || topology > 1)
                return false;

            const std::uint32_t current_delay = m_delay.load(std::memory_order_acquire);
            const std::uint32_t current_topology = m_topology_mode.load(std::memory_order_acquire);

            if (m_max_players <= 2)
            {
                for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
                    controllers[i] = i + 1;
            }
            if (!ValidateControllerMap(controllers))
                return false;

            const auto current_controllers = CurrentControllerMap();
            if (delay == current_delay && topology == current_topology &&
                controllers == current_controllers)
                return true;

            if (VMManager::HasValidVM() && topology != current_topology)
            {
                SetLastError("游戏运行中可以实时修改输入延迟；多人手柄布局请切换游戏后调整");
                return false;
            }

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
                m_runtime_required_acks.fill(false);
                for (std::uint32_t i = 0; i < m_room_capacity; i++)
                    m_runtime_required_acks[i] = m_players[i].connected;
            }

            if (!StageRuntimeConfigLocal(change_id, delay, topology, controllers))
                return false;
            const auto payload = BuildRuntimeConfig(change_id, delay, topology, controllers);
            BroadcastControl(ControlType::RuntimeApply, payload.data(),
                static_cast<std::uint32_t>(payload.size()));
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_runtime_acks[0] = true;
            }
            BroadcastRoster();
            MaybeCommitRuntimeConfig();
            return true;
        }

        bool RequestRuntimeSettings(std::uint32_t local_controller,
            std::uint32_t delay, std::uint32_t topology)
        {
            if (m_role == Role::Client)
            {
                if (m_max_players <= 2)
                    return true;
                if (local_controller < 1 || local_controller > m_max_players)
                    return false;
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
            if (m_max_players >= 3)
            {
                if (local_controller < 1 || local_controller > m_max_players)
                    return false;
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
            }
            return BeginHostRuntimeConfig(delay, topology, controllers);
        }

        void HandleRuntimeRequest(std::uint32_t requester_id, std::uint32_t desired_controller)
        {
            if (m_role != Role::Host || m_max_players <= 2 ||
                requester_id < 1 || requester_id > m_max_players ||
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

        bool RequestRoomCapacity(std::uint32_t capacity)
        {
            if (m_role != Role::Host || capacity < 1 || capacity > MAX_PLAYERS)
                return false;
            std::uint32_t previous = 0;
            bool start_listener = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                previous = m_room_capacity;
                if (capacity < previous)
                {
                    m_last_error = "当前会话只支持实时增加人数，不支持缩小房间";
                    return false;
                }
                if (capacity == previous)
                    return true;
                m_room_capacity = capacity;
                for (std::uint32_t i = previous; i < capacity; i++)
                    m_players[i].controller = i + 1;
                if (!m_round_in_progress && !m_start_committed && !VMManager::HasValidVM())
                    m_max_players = capacity;
                start_listener = (previous == 1 && capacity > 1 && !m_connector.joinable());
                m_connecting.store(RoomConnectedCountLocked() < m_room_capacity,
                    std::memory_order_release);
            }
            if (start_listener)
                m_connector = std::thread([this]() { AcceptLoop(); });
            Log("room capacity expanded: %u -> %u; active round=%u",
                static_cast<unsigned>(previous), static_cast<unsigned>(capacity),
                static_cast<unsigned>(m_max_players));
            Host::AddKeyedOSDMessage("ModernNetplayCapacity",
                "联机房间已扩容；新玩家可直接加入，当前本局不会被打断。", 7.0f);
            BroadcastRoster();
            return true;
        }

        void ReturnToLobbyLocal(const char* reason)
        {
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_game_switching = true;
                m_round_in_progress = false;
                m_game_selected = false;
                m_local_game_match = false;
                m_game_title.clear();
                m_game_serial.clear();
                m_game_crc = 0;
                m_local_game_path.clear();
                m_game_is_arcade = false;
                m_game_fingerprint = {};
                ResetStartStateLocked();
                m_max_players = m_room_capacity;
                for (std::uint32_t i = 0; i < MAX_PLAYERS; i++)
                {
                    m_players[i].game_match = false;
                    m_players[i].controller = i + 1;
                }
                m_last_error = reason ? reason : "已返回联机房间";
            }
            std::uint32_t epoch = m_input_epoch.fetch_add(1, std::memory_order_acq_rel) + 1u;
            if (epoch == 0)
                m_input_epoch.store(1, std::memory_order_release);
            m_frame_counter.store(0, std::memory_order_release);
            m_input_cv.notify_all();
            m_bundle_cv.notify_all();
            m_boot_cv.notify_all();
            m_first_poll_cv.notify_all();
            Host::RunOnCPUThread([]() {
                if (VMManager::HasValidVM())
                    VMManager::SetState(VMState::Stopping);
            });
            Host::AddKeyedOSDMessage("ModernNetplayReturnLobby",
                "当前游戏已同步结束，联机房间和连接保持。", 7.0f);
        }

        bool RequestReturnToLobby()
        {
            if (m_role != Role::Host)
                return false;
            BroadcastControl(ControlType::RoundEnd, nullptr, 0);
            ReturnToLobbyLocal("已同步结束当前游戏，可直接选择下一款游戏");
            BroadcastRoster();
            return true;
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

            const bool desync = (reason.find("不同步") != std::string::npos);
            Host::RunOnCPUThread([message, desync]() {
                if (VMManager::HasValidVM())
                {
                    VMManager::SetState(VMState::Paused);
                    Host::AddKeyedOSDMessage("ModernNetplayDisconnected",
                        desync ? "检测到联机不同步，游戏已暂停。" : "联机已中断，游戏已暂停。",
                        Host::OSD_CRITICAL_ERROR_DURATION);
                }
            });
            Host::ReportErrorAsync(desync ? "PCSX2 检测到联机不同步" : "PCSX2 联机已中断", message);
        }

        bool SendArcadeJvsInputToHost(std::uint32_t frame, const ArcadeJvsState& state)
        {
            std::array<std::uint8_t, 12 + ARCADE_JVS_STATE_BYTES> payload{};
            WriteU32(payload.data() + 0, frame);
            WriteU32(payload.data() + 4, m_local_player_id);
            WriteU32(payload.data() + 8, m_input_epoch.load(std::memory_order_acquire));
            WriteArcadeJvsState(payload.data() + 12, state);
            return SendControlToHost(ControlType::ArcadeJvsInput, payload.data(),
                static_cast<std::uint32_t>(payload.size()));
        }

        void BroadcastArcadeJvsBundle(std::uint32_t frame, const ArcadeJvsBundle& bundle)
        {
            std::array<std::uint8_t, 12 + (2 * ARCADE_JVS_STATE_BYTES)> payload{};
            WriteU32(payload.data() + 0, frame);
            WriteU32(payload.data() + 4, std::min<std::uint32_t>(m_max_players, 2u));
            WriteU32(payload.data() + 8, m_input_epoch.load(std::memory_order_acquire));
            WriteArcadeJvsState(payload.data() + 12, bundle[0]);
            WriteArcadeJvsState(payload.data() + 12 + ARCADE_JVS_STATE_BYTES, bundle[1]);
            BroadcastControl(ControlType::ArcadeJvsBundle, payload.data(),
                static_cast<std::uint32_t>(payload.size()));
        }

        void HandleArcadeJvsInput(Peer& peer, const std::vector<std::uint8_t>& payload)
        {
            if (payload.size() != (12 + ARCADE_JVS_STATE_BYTES))
                return;
            const std::uint32_t frame = ReadU32(payload.data() + 0);
            const std::uint32_t player_id = ReadU32(payload.data() + 4);
            const std::uint32_t epoch = ReadU32(payload.data() + 8);
            if (player_id != peer.player_id || player_id < 2 || player_id > 2 ||
                epoch != m_input_epoch.load(std::memory_order_acquire))
                return;
            const ArcadeJvsState state = ReadArcadeJvsState(payload.data() + 12);
            {
                std::lock_guard<std::mutex> lock(m_input_mutex);
                m_arcade_jvs_inputs[player_id - 1][frame] = state;
            }
            m_input_cv.notify_all();
        }

        void HandleArcadeJvsBundle(const std::vector<std::uint8_t>& payload)
        {
            if (payload.size() != (12 + (2 * ARCADE_JVS_STATE_BYTES)))
                return;
            const std::uint32_t frame = ReadU32(payload.data() + 0);
            const std::uint32_t count = ReadU32(payload.data() + 4);
            const std::uint32_t epoch = ReadU32(payload.data() + 8);
            if (count < 1 || count > 2 || epoch != m_input_epoch.load(std::memory_order_acquire))
                return;
            ArcadeJvsBundle bundle{};
            bundle[0] = ReadArcadeJvsState(payload.data() + 12);
            bundle[1] = ReadArcadeJvsState(payload.data() + 12 + ARCADE_JVS_STATE_BYTES);
            {
                std::lock_guard<std::mutex> lock(m_bundle_mutex);
                m_arcade_jvs_bundles[frame] = bundle;
            }
            m_bundle_cv.notify_all();
        }

        bool WaitForArcadeJvsHostInputs(std::uint32_t frame, ArcadeJvsBundle* bundle)
        {
            const auto started = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(m_input_mutex);
            const bool ready = m_input_cv.wait_for(lock, std::chrono::seconds(RECEIVE_TIMEOUT_SECONDS),
                [this, frame]() {
                    for (std::uint32_t i = 0; i < m_max_players; i++)
                    {
                        if (m_arcade_jvs_inputs[i].find(frame) == m_arcade_jvs_inputs[i].end())
                            return m_failed.load(std::memory_order_acquire) ||
                                m_stop_requested.load(std::memory_order_acquire);
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
                const auto it = m_arcade_jvs_inputs[i].find(frame);
                if (it == m_arcade_jvs_inputs[i].end())
                    return false;
                (*bundle)[i] = it->second;
            }
            return true;
        }

        bool WaitForArcadeJvsBundle(std::uint32_t frame, ArcadeJvsBundle* bundle)
        {
            const auto started = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(m_bundle_mutex);
            const bool ready = m_bundle_cv.wait_for(lock, std::chrono::seconds(RECEIVE_TIMEOUT_SECONDS),
                [this, frame]() {
                    return m_arcade_jvs_bundles.find(frame) != m_arcade_jvs_bundles.end() ||
                        m_failed.load(std::memory_order_acquire) ||
                        m_stop_requested.load(std::memory_order_acquire) ||
                        !m_connected.load(std::memory_order_acquire);
                });
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            LogStall(frame, waited);
            if (!ready)
                return false;
            const auto it = m_arcade_jvs_bundles.find(frame);
            if (it == m_arcade_jvs_bundles.end())
                return false;
            *bundle = it->second;
            return true;
        }

        void PruneArcadeJvsHistory()
        {
            const std::uint32_t delay = m_delay.load(std::memory_order_acquire);
            const std::uint32_t keep_window = std::max<std::uint32_t>(240u, delay + 180u);
            if (m_frame < keep_window)
                return;
            const std::uint32_t keep_from = m_frame - keep_window;
            {
                std::lock_guard<std::mutex> lock(m_input_mutex);
                for (auto& map : m_arcade_jvs_inputs)
                {
                    for (auto it = map.begin(); it != map.end();)
                        it = (it->first < keep_from) ? map.erase(it) : std::next(it);
                }
            }
            {
                std::lock_guard<std::mutex> lock(m_bundle_mutex);
                for (auto it = m_arcade_jvs_bundles.begin(); it != m_arcade_jvs_bundles.end();)
                    it = (it->first < keep_from) ? m_arcade_jvs_bundles.erase(it) : std::next(it);
            }
        }

        bool SynchronizeArcadeJvsInternal(const ArcadeJvsState& local_state, ArcadeJvsBundle* output)
        {
            if (!output || !IsConfigured() || !ACJV::enabled)
                return false;
            if (!m_start_committed || !m_connected.load(std::memory_order_acquire))
                return false;
            if (m_max_players > 2)
            {
                const std::string reason =
                    "System 246/256 JVS 当前一次只支持 1～2 个联机玩家；请将本局人数设为 2 人以内";
                if (!m_failed.load(std::memory_order_acquire))
                {
                    if (m_role == Role::Host)
                        BroadcastSessionAbort(reason);
                    Fail(reason.c_str());
                }
                return false;
            }
            if (!WaitForFirstPollBarrier())
                return false;

            ApplyPendingRuntimeConfigIfDue();

            const std::uint32_t input_frame = m_frame;
            if (m_role == Role::Host)
            {
                {
                    std::lock_guard<std::mutex> lock(m_input_mutex);
                    m_arcade_jvs_inputs[0][input_frame] = local_state;
                }
                m_input_cv.notify_all();
            }
            else if (!SendArcadeJvsInputToHost(input_frame, local_state))
            {
                Fail("failed to send System 246/256 JVS input");
                return false;
            }

            ArcadeJvsBundle authoritative{};
            authoritative[0].mode = local_state.mode;
            authoritative[1].mode = local_state.mode;

            const std::uint32_t delay = m_delay.load(std::memory_order_acquire);
            const std::uint32_t source_frame = (input_frame >= delay) ? (input_frame - delay) : input_frame;
            if (m_role == Role::Host)
            {
                if (!WaitForArcadeJvsHostInputs(source_frame, &authoritative))
                {
                    const std::string reason = "等待 System 246/256 JVS 输入超时，联机无法继续保持同步";
                    BroadcastSessionAbort(reason);
                    Fail(reason.c_str());
                    return false;
                }
                if ((source_frame % LOG_FRAME_INTERVAL) == 0)
                    RecordHostSyncCheckpoint(source_frame, static_cast<std::uint64_t>(g_FrameCount));
                BroadcastArcadeJvsBundle(source_frame, authoritative);
            }
            else
            {
                if (!WaitForArcadeJvsBundle(source_frame, &authoritative))
                {
                    Fail("客户端等待房主 JVS 权威输入超时");
                    return false;
                }
                if ((source_frame % LOG_FRAME_INTERVAL) == 0)
                    SendClientSyncCheckpoint(source_frame, static_cast<std::uint64_t>(g_FrameCount));
            }

            *output = authoritative;
            ++m_frame;
            m_frame_counter.store(m_frame, std::memory_order_release);
            if ((source_frame % LOG_FRAME_INTERVAL) == 0)
                Log("ARCADE_SYNC poll=%u vsync=%llu delay=%u players=%u mode=%u",
                    static_cast<unsigned>(source_frame),
                    static_cast<unsigned long long>(g_FrameCount),
                    static_cast<unsigned>(delay),
                    static_cast<unsigned>(m_max_players),
                    static_cast<unsigned>(local_state.mode));
            PruneArcadeJvsHistory();
            return true;
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

        void RecordHostSyncCheckpoint(std::uint32_t poll, std::uint64_t vsync)
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_host_sync_checkpoints[poll] = vsync;
            if (poll >= (LOG_FRAME_INTERVAL * 20u))
                m_host_sync_checkpoints.erase(poll - (LOG_FRAME_INTERVAL * 20u));
        }

        void SendClientSyncCheckpoint(std::uint32_t poll, std::uint64_t vsync)
        {
            std::array<std::uint8_t, 12> payload{};
            WriteU32(payload.data(), poll);
            WriteU64(payload.data() + 4, vsync);
            if (!SendControlToHost(ControlType::SyncCheckpoint, payload.data(),
                    static_cast<std::uint32_t>(payload.size())))
                Fail("failed to send synchronization checkpoint");
        }

        void HandlePeerSyncCheckpoint(std::uint32_t player_id, const std::vector<std::uint8_t>& payload)
        {
            if (m_role != Role::Host || player_id < 2 || player_id > m_max_players || payload.size() != 12)
                return;

            const std::uint32_t poll = ReadU32(payload.data());
            const std::uint64_t peer_vsync = ReadU64(payload.data() + 4);
            std::uint64_t host_vsync = 0;
            std::uint32_t strikes = 0;
            bool trigger = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                const auto it = m_host_sync_checkpoints.find(poll);
                if (it == m_host_sync_checkpoints.end())
                    return;
                host_vsync = it->second;
                const std::uint64_t delta = (host_vsync >= peer_vsync) ?
                    (host_vsync - peer_vsync) : (peer_vsync - host_vsync);
                std::uint32_t& counter = m_desync_strikes[player_id - 1];
                counter = (delta >= 2u) ? (counter + 1u) : 0u;
                strikes = counter;
                trigger = (counter >= 3u);
            }

            const long long signed_delta = static_cast<long long>(host_vsync) -
                static_cast<long long>(peer_vsync);
            Log("SYNC_CHECK P%u poll=%u host_vsync=%llu peer_vsync=%llu delta=%lld strikes=%u",
                static_cast<unsigned>(player_id), static_cast<unsigned>(poll),
                static_cast<unsigned long long>(host_vsync),
                static_cast<unsigned long long>(peer_vsync),
                signed_delta, static_cast<unsigned>(strikes));

            if (trigger && !m_failed.load(std::memory_order_acquire))
            {
                const std::string reason =
                    "检测到玩家 P" + std::to_string(player_id) +
                    " 已不同步：同一输入检查点的 VSync 连续偏移。游戏已自动暂停，请重新同步启动。";
                BroadcastSessionAbort(reason);
                Fail(reason.c_str());
            }
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
            ApplyPendingRuntimeConfigIfDue();

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
                m_frame_counter.store(m_frame, std::memory_order_release);
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
                    if ((source_frame % LOG_FRAME_INTERVAL) == 0)
                        RecordHostSyncCheckpoint(source_frame, static_cast<std::uint64_t>(g_FrameCount));
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
                {
                    const std::uint64_t vsync = static_cast<std::uint64_t>(g_FrameCount);
                    Log("SYNC poll=%u vsync=%llu delay=%u players=%u wait_ms=%u",
                        static_cast<unsigned>(source_frame), static_cast<unsigned long long>(vsync),
                        static_cast<unsigned>(delay), static_cast<unsigned>(m_max_players),
                        static_cast<unsigned>(m_last_wait_ms.load(std::memory_order_acquire)));
                    if (m_role == Role::Client)
                        SendClientSyncCheckpoint(source_frame, vsync);
                }
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
                fatal_disconnect = (dropped_player_id <= m_max_players) &&
                    (m_prepare_boot || m_start_committed);
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
                        if (accepted && AllMemcardsReadyLocked())
                        {
                            m_memory_card_transfer_active = false;
                            m_memory_card_failed = false;
                            m_memory_card_status = "全员记忆卡同步完成 ✓";
                        }
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
                case ControlType::ArcadeJvsInput:
                    HandleArcadeJvsInput(peer, payload);
                    break;
                case ControlType::SyncCheckpoint:
                    HandlePeerSyncCheckpoint(peer.player_id, payload);
                    break;
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
                case ControlType::MemcardAbort:
                    HandleClientMemcardAbort(payload);
                    break;
                case ControlType::PrepareBoot:
                    HandleClientPrepareBoot();
                    break;
                case ControlType::StartCommit:
                {
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        m_start_committed = true;
                        m_round_in_progress = true;
                        m_game_switching = false;
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
                case ControlType::ArcadeJvsBundle:
                    HandleArcadeJvsBundle(payload);
                    break;
                case ControlType::RuntimeApply:
                {
                    std::uint32_t change_id = 0;
                    std::uint32_t delay = 0;
                    std::uint32_t topology = 0;
                    std::array<std::uint32_t, MAX_PLAYERS> controllers{};
                    if (!ParseRuntimeConfig(payload, &change_id, &delay, &topology, &controllers) ||
                        !StageRuntimeConfigLocal(change_id, delay, topology, controllers))
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
                    if (payload.size() == 8)
                        ScheduleRuntimeConfigCommit(ReadU32(payload.data()), ReadU32(payload.data() + 4));
                    break;
                }
                case ControlType::RoundEnd:
                    ReturnToLobbyLocal("房主已同步结束当前游戏，等待下一局");
                    break;
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
        std::uint32_t m_room_capacity = 2;
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
        std::array<bool, MAX_PLAYERS> m_runtime_required_acks{};
        std::uint32_t m_pending_runtime_delay = 2;
        std::uint32_t m_pending_runtime_topology = 1;
        std::array<std::uint32_t, MAX_PLAYERS> m_pending_runtime_controllers{1, 2, 3, 4};
        std::uint32_t m_runtime_commit_frame = 0;
        bool m_runtime_commit_pending = false;
        bool m_game_selected = false;
        bool m_local_game_match = false;
        std::string m_game_title;
        std::string m_game_serial;
        std::uint32_t m_game_crc = 0;
        std::string m_local_game_path;
        bool m_game_is_arcade = false;
        DeterminismFingerprint m_game_fingerprint{};

        bool m_start_requested = false;
        bool m_memcard_transfer_started = false;
        bool m_memory_card_local_ready = false;
        bool m_memory_card_present = false;
        bool m_memory_card_transfer_active = false;
        bool m_memory_card_failed = false;
        std::uint64_t m_memory_card_crc = 0;
        std::uint32_t m_memory_card_size = 0;
        std::uint64_t m_memory_card_transferred_bytes = 0;
        std::string m_memory_card_status = "等待记忆卡同步";
        std::string m_shadow_card_filename;
        std::string m_shadow_arcade_sram_path;
        std::vector<std::uint8_t> m_memcard_receive;
        std::uint64_t m_memcard_received_bytes = 0;

        bool m_prepare_boot = false;
        bool m_boot_launch_pending = false;
        bool m_boot_launch_consumed = false;
        bool m_local_boot_ready = false;
        bool m_start_committed = false;
        bool m_first_poll_go = false;
        bool m_round_in_progress = false;
        bool m_game_switching = false;

        std::uint32_t m_frame = 0;
        std::atomic<std::uint32_t> m_frame_counter{0};
        bool m_have_capture = false;
        InputFrame m_capture = NEUTRAL_FRAME;
        InputFrame m_last_logged_local_input = NEUTRAL_FRAME;
        InputBundle m_output_bundle = {NEUTRAL_FRAME, NEUTRAL_FRAME, NEUTRAL_FRAME, NEUTRAL_FRAME};
        std::array<std::unordered_map<std::uint32_t, InputFrame>, MAX_PLAYERS> m_player_inputs;
        std::unordered_map<std::uint32_t, InputBundle> m_bundles;
        std::array<std::unordered_map<std::uint32_t, ArcadeJvsState>, 2> m_arcade_jvs_inputs;
        std::unordered_map<std::uint32_t, ArcadeJvsBundle> m_arcade_jvs_bundles;
        std::unordered_map<std::uint32_t, std::uint64_t> m_host_sync_checkpoints;
        std::array<std::uint32_t, MAX_PLAYERS> m_desync_strikes{};

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
    return GetSession().RequestRuntimeSettingsImpl(local_controller, delay, topology_mode);
}
bool RequestRoomCapacity(std::uint32_t max_players) { return GetSession().RequestRoomCapacityImpl(max_players); }
bool RequestReturnToLobby() { return GetSession().RequestReturnToLobbyImpl(); }
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
bool SynchronizeArcadeJvs(const ArcadeJvsState& local_state, ArcadeJvsBundle* bundle)
{
    return GetSession().SynchronizeArcadeJvsImpl(local_state, bundle);
}
std::string GetArcadeDongleOverride(const std::string& original_filename)
{
    return GetSession().GetArcadeDongleOverrideImpl(original_filename);
}
std::string GetArcadeSramOverride(const std::string& original_path)
{
    return GetSession().GetArcadeSramOverrideImpl(original_path);
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
bool RequestRoomCapacity(std::uint32_t) { return false; }
bool RequestReturnToLobby() { return false; }
bool CanStartVM() { return true; }
bool ShouldHoldBootBarrier() { return false; }
void NotifyBootReady() {}
bool WaitForStartCommit() { return true; }
void ApplyDeterministicConfig() {}
bool ShouldForceDualShock2Slot(std::uint32_t) { return false; }
bool ShouldDisconnectControllerSlot(std::uint32_t) { return false; }
std::uint8_t HandlePadResponse(std::uint8_t, std::uint32_t, std::uint8_t local_value) { return local_value; }
bool SynchronizeArcadeJvs(const ArcadeJvsState&, ArcadeJvsBundle*) { return false; }
std::string GetArcadeDongleOverride(const std::string& original_filename) { return original_filename; }
std::string GetArcadeSramOverride(const std::string& original_path) { return original_path; }
void Shutdown() {}
#endif
} // namespace ModernNetplay
