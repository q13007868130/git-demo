# PCSX2 Modern Netplay v0.1 test guide

This first build is deliberately a proof-of-port of the classic PCSX2 Online input model onto current PCSX2. It is not yet the final room-browser/relay release.

## What v0.1 already does

- Builds current `PCSX2/pcsx2` master instead of an old emulator core.
- Intercepts the exact DS2 `POLL` response bytes used by the legacy 1.5-era Online implementation: 2 digital button bytes and 4 analog-stick bytes.
- Uses two-player delay-based lockstep.
- Host is virtual controller 1; joining player is virtual controller 2.
- Each player binds only their normal local controller 1. The client input is captured from local controller 1 and injected into virtual controller 2 automatically.
- Controller port 2 is made present automatically in Netplay mode.
- Host controls the input-delay value.

## First test

Use the same PCSX2 Netplay build, same game region/image, BIOS family, and compatible emulator settings on both PCs. Tekken 5 is the first recommended test because it is a straightforward local two-player title and was a known target of the old PCSX2 Online work.

Host runs `Netplay-Host.bat`. Joiner runs `Netplay-Join.bat` and enters the host IPv4 address or hostname. Both then start the same game. The host may appear paused at the first controller poll until the client connects; this is intentional in v0.1.

Default transport is TCP 27886. For Internet testing the host must be reachable on that port (router forwarding, public IPv4, or a VPN/overlay network can be used). LAN testing needs no router forwarding.

## Expected limitations

v0.1 does not yet synchronize pressure-sensitive bytes 9-20, memory cards, savestates, game/config hashes, or full emulator-state hashes. It also does not yet provide rollback, NAT traversal, relay, matchmaking, room codes, chat, or a Qt host/join dialog. These are intentionally left out until the modern SIO/PAD synchronization path is proven in real gameplay.

If gameplay diverges, keep both emulator logs. The next milestone adds periodic desync hashes and game/config handshake validation before expanding the UI.
