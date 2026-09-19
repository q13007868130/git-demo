// SPDX-FileCopyrightText: 2026 PCSX2 Modern Netplay Port contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace ModernNetplay
{
    static constexpr std::uint32_t MAX_PLAYERS = 4;

    struct ArcadeJvsState
    {
        std::uint32_t mode = 0;
        std::uint16_t buttons = 0;
        std::uint16_t coin = 0;
        std::uint16_t screen_x = 0xffff;
        std::uint16_t screen_y = 0xffff;
        std::uint16_t raw_x = 0xffff;
        std::uint16_t raw_y = 0xffff;
        std::array<std::uint16_t, 4> drum{};
        std::array<std::uint16_t, 3> analog{0x8000, 0, 0};
        std::uint16_t dip_switch_state = 0;
        std::uint16_t test_state = 0;
        std::uint16_t flags = 0;
        std::uint32_t reserved = 0;
    };

    using ArcadeJvsBundle = std::array<ArcadeJvsState, 2>;

    struct PlayerSnapshot
    {
        std::uint32_t id = 0;
        bool connected = false;
        bool game_match = false;
        bool memcard_ready = false;
        bool boot_ready = false;
        std::uint32_t controller = 0;
        std::string name;
    };

    struct StatusSnapshot
    {
        bool configured = false;
        bool connecting = false;
        bool connected = false;
        bool failed = false;
        bool room_full = false;
        std::uint32_t player_count = 0;
        std::uint32_t max_players = 2;
        std::uint32_t round_players = 2;
        std::uint32_t local_player_id = 0;
        std::uint32_t delay = 2;
        std::uint16_t port = 27886;
        std::string role;
        std::string username;
        std::string last_error;
        std::string log_path;
        std::uint64_t session_id = 0;

        std::array<PlayerSnapshot, MAX_PLAYERS> players{};

        bool game_selected = false;
        bool local_game_match = false;
        bool all_games_match = false;
        std::string game_title;
        std::string game_serial;
        std::uint32_t game_crc = 0;
        std::string local_game_path;

        bool memory_card_sync_enabled = true;
        bool memory_card_local_ready = false;
        bool memory_card_all_ready = false;
        bool memory_card_present = false;
        bool memory_card_transfer_active = false;
        bool memory_card_failed = false;
        std::uint64_t memory_card_crc = 0;
        std::uint32_t memory_card_size = 0;
        std::uint64_t memory_card_transferred_bytes = 0;
        std::string memory_card_status;

        bool start_requested = false;
        bool prepare_boot = false;
        bool local_boot_ready = false;
        bool all_boot_ready = false;
        bool start_committed = false;
        bool first_poll_released = false;
        bool round_in_progress = false;
        bool game_switching = false;
        std::uint32_t topology_mode = 1;
        bool runtime_reconfiguring = false;
        std::uint32_t input_epoch = 0;
    };

    // True for binaries produced by this Modern Netplay build pipeline.
    // Used to prevent the stock PCSX2 updater from overwriting the Netplay build.
    bool IsCustomBuild();
    bool IsConfigured();
    void StartSessionAsync();
    // Rebuild only the Netplay backend from the current environment settings.
    // This makes a failed/disconnected lobby reusable without restarting PCSX2.
    bool RestartSession();
    StatusSnapshot GetStatusSnapshot();

    bool HostSelectGame(const std::string& path, const std::string& title,
        const std::string& serial, std::uint32_t crc);

    // Host-only. The request may be issued immediately after HostSelectGame().
    // It remains pending until all configured players have matched the game and
    // the shadow memory-card transfer has completed, then PREPARE_BOOT is sent.
    bool RequestSynchronizedBoot();
    bool ConsumeBootLaunchRequest(std::string* path);

    // Live Netplay controls. Every peer may request its logical P1-P4 controller.
    // Delay/topology are host-authoritative and are locked once a VM is running.
    bool RequestRuntimeSettings(std::uint32_t local_controller,
        std::uint32_t delay, std::uint32_t topology_mode);
    bool RequestRoomCapacity(std::uint32_t max_players);
    bool RequestReturnToLobby();

    bool CanStartVM();
    bool ShouldHoldBootBarrier();
    void NotifyBootReady();
    bool WaitForStartCommit();

    // Applied during VM settings load. This redirects slot 1 to the synchronized
    // shadow card (without touching the user's original card), configures the
    // multitap for 3-4 player rooms, and disables unused virtual pad slots.
    void ApplyDeterministicConfig();
    bool ShouldForceDualShock2Slot(std::uint32_t unified_slot);
    bool ShouldDisconnectControllerSlot(std::uint32_t unified_slot);

    // Synchronizes the complete DualShock 2 POLL response (bytes 3..20):
    // digital buttons, analog sticks, and all pressure-sensitive buttons.
    std::uint8_t HandlePadResponse(std::uint8_t unified_slot,
        std::uint32_t command_index, std::uint8_t local_value);

    // PCSX2X6/System 246-256 JVS path. Each cabinet connection contributes its
    // own logical JVS player; the host returns the same authoritative two-player
    // state to every VM before the emulated JVS board answers the game.
    bool SynchronizeArcadeJvs(const ArcadeJvsState& local_state, ArcadeJvsBundle* bundle);

    void Shutdown();
} // namespace ModernNetplay
