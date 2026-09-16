# Modern Netplay port notes

## Confirmed legacy design

The PCSX2 1.5-era Online Plus implementation synchronizes the DualShock response at the SIO/PAD boundary. For normal poll command 0x42 it exchanges six response bytes: two digital button bytes and four analog-stick bytes. The network session provides an input-delay lockstep and compares initial emulator state before starting.

## Confirmed current PCSX2 hook point

Current PCSX2 still represents DualShock 2 traffic with `PadDualshock2::SendCommandByte()`, `Pad::Command::POLL` (0x42), `commandBytesReceived`, and `PadDualshock2::Poll()`. This provides a direct modern equivalent of the old IOP/PAD shim without replacing the current SDL/InputManager binding system.

## Port strategy

1. Keep current PCSX2 master as the emulator base.
2. Implement a new GPL-3.0+ Netplay service instead of copying the old wxWidgets UI.
3. Hook only the DS2 poll response path for the first milestone.
4. Start with two-player delay-based lockstep and deterministic session checks.
5. Add Qt host/join UI after the core is proven.

The initial Windows CI build is intentionally a clean upstream checkpoint. The following commits layer the Netplay core onto that known-good build.
