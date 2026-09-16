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
	};

	// True when PCSX2 was launched with PCSX2_NETPLAY_MODE=host/client.
	bool IsConfigured();

	// Starts the host accept/client connect path on a background thread so the
	// Netplay dialog can behave like a real 1/2 -> 2/2 lobby before a game boots.
	void StartSessionAsync();

	// Lightweight thread-safe state used by the Qt lobby/status panel.
	StatusSnapshot GetStatusSnapshot();

	// v0.4 deliberately keeps the proven v0.1 input semantics. Determinism checks
	// are diagnostic-only until the logs prove which settings are safe to enforce.
	void ApplyDeterministicConfig();

	// Intercepts the first six DualShock 2 POLL response bytes (two digital + four analog),
	// matching the synchronization boundary used by the old PCSX2 Online/1.5-era netplay code.
	std::uint8_t HandlePadResponse(std::uint8_t unified_slot, std::uint32_t command_index, std::uint8_t local_value);

	// Stops networking and wakes any emulation thread blocked waiting for a peer frame.
	void Shutdown();
} // namespace ModernNetplay
