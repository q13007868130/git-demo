// SPDX-FileCopyrightText: 2026 PCSX2 Modern Netplay Port contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <string>

namespace ModernNetplay
{
    struct StatusSnapshot
    {
        bool configured = false;
        bool connecting = false;
        bool connected = false;
        bool failed = false;
        std::uint32_t player_count = 0;
        std::uint32_t delay = 2;
        std::uint16_t port = 27886;
        std::string role;
        std::string peer;
        std::string last_error;
        std::string log_path;

        bool game_selected = false;
        bool local_game_match = false;
        bool peer_game_match = false;
        std::string game_title;
        std::string game_serial;
        std::uint32_t game_crc = 0;
        std::string local_game_path;

        bool prepare_boot = false;
        bool local_boot_ready = false;
        bool peer_boot_ready = false;
        bool start_committed = false;
        bool first_poll_released = false;
    };

    bool IsConfigured();
    void StartSessionAsync();
    StatusSnapshot GetStatusSnapshot();

    // Host announces the exact local game selected in the room. The path is
    // local-only; peers receive title/serial/CRC and resolve their own image.
    bool HostSelectGame(const std::string& path, const std::string& title,
        const std::string& serial, std::uint32_t crc);

    // Host-only synchronized boot request. Both peers launch their matching
    // local image, initialize the VM, wait at BOOT_READY, then enter Running
    // only after the host broadcasts START_COMMIT.
    bool RequestSynchronizedBoot();
    bool ConsumeBootLaunchRequest(std::string* path);

    // EmuThread integration used to prevent normal/double-click boot from
    // bypassing the Netplay room and to hold initialized VMs at the barrier.
    bool CanStartVM();
    bool ShouldHoldBootBarrier();
    void NotifyBootReady();
    bool WaitForStartCommit();

    void ApplyDeterministicConfig();

    // Intercepts the first six DualShock 2 POLL response bytes (two digital +
    // four analog), preserving the working v0.1 lockstep input semantics.
    std::uint8_t HandlePadResponse(std::uint8_t unified_slot,
        std::uint32_t command_index, std::uint8_t local_value);

    void Shutdown();
} // namespace ModernNetplay
