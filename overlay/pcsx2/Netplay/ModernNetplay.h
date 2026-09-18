// SPDX-FileCopyrightText: 2026 PCSX2 Modern Netplay Port contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace ModernNetplay
{
    static constexpr std::uint32_t MAX_PLAYERS = 4;

    struct PlayerSnapshot
    {
        std::uint32_t id = 0;
        bool connected = false;
        bool game_match = false;
        bool memcard_ready = false;
        bool boot_ready = false;
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
        std::uint64_t memory_card_crc = 0;
        std::uint32_t memory_card_size = 0;
        std::string memory_card_status;

        bool start_requested = false;
        bool prepare_boot = false;
        bool local_boot_ready = false;
        bool all_boot_ready = false;
        bool start_committed = false;
        bool first_poll_released = false;
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

    void Shutdown();
} // namespace ModernNetplay
