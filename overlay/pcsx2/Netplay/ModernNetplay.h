// SPDX-FileCopyrightText: 2026 PCSX2 Modern Netplay Port contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>

namespace ModernNetplay
{
	// True when PCSX2 was launched with PCSX2_NETPLAY_MODE=host/client.
	// Used to make controller port 2 present without requiring a second local binding.
	bool IsConfigured();

	// Applies the conservative deterministic profile used by Netplay before VM startup.
	// This normalizes core timing/CPU settings, disables unsafe patches/cheats and host-
	// dependent RTC/memory-card inputs which can make two VMs drift despite identical pads.
	void ApplyDeterministicConfig();

	// Intercepts the first six DualShock 2 POLL response bytes (two digital + four analog),
	// matching the synchronization boundary used by the old PCSX2 Online/1.5-era netplay code.
	std::uint8_t HandlePadResponse(std::uint8_t unified_slot, std::uint32_t command_index, std::uint8_t local_value);

	// Stops networking and wakes any emulation thread blocked waiting for a peer frame.
	void Shutdown();
} // namespace ModernNetplay