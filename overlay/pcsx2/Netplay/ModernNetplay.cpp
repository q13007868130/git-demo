// SPDX-FileCopyrightText: 2026 PCSX2 Modern Netplay Port contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Netplay/ModernNetplay.h"

#include "Config.h"
#include "Counters.h"
#include "GS/GSXXH.h"
#include "IopMem.h"
#include "Memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

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
	constexpr std::uint32_t HELLO_MAGIC = 0x50324E50; // P2NP
	constexpr std::uint32_t FRAME_MAGIC = 0x46524D45; // FRME
	constexpr std::uint32_t PROTOCOL_VERSION = 2;
	constexpr std::uint16_t DEFAULT_PORT = 27886;
	constexpr int RECEIVE_TIMEOUT_SECONDS = 10;
	constexpr std::uint32_t STATE_HASH_INTERVAL = 120;
	constexpr InputFrame NEUTRAL_FRAME = {0xff, 0xff, 0x7f, 0x7f, 0x7f, 0x7f};

	struct InputSample
	{
		InputFrame input = NEUTRAL_FRAME;
		std::uint32_t emu_frame = 0;
		std::uint64_t state_hash = 0;
	};

	enum class Role : std::uint32_t
	{
		Disabled = 0,
		Host = 1,
		Client = 2,
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
		WriteU32(dst + 4, static_cast<std::uint32_t>(value));
	}

	std::uint64_t ReadU64(const std::uint8_t* src)
	{
		return (static_cast<std::uint64_t>(ReadU32(src)) << 32) |
			static_cast<std::uint64_t>(ReadU32(src + 4));
	}

	std::uint64_t HashVmMemory()
	{
		const std::uint64_t ee_hash = GSXXH3_64bits(eeMem->Main, Ps2MemSize::ExposedRam);
		const std::uint64_t iop_hash = GSXXH3_64bits(iopMem->Main, Ps2MemSize::ExposedIopRam);
		return ee_hash ^ (iop_hash + 0x9e3779b97f4a7c15ULL + (ee_hash << 6) + (ee_hash >> 2));
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
		}

		~Session()
		{
			Stop();
		}

		bool IsConfigured() const
		{
			return m_role != Role::Disabled;
		}

		std::uint8_t HandlePadResponse(std::uint8_t unified_slot, std::uint32_t command_index, std::uint8_t local_value)
		{
			if (m_role == Role::Disabled || command_index < 3 || command_index > 8)
				return local_value;

			const std::size_t input_index = static_cast<std::size_t>(command_index - 3);

			// Port 1 is the local physical-input source on both peers. On the client its
			// bytes are captured before being replaced with host data, then replayed as
			// virtual controller port 2. This means the user only needs one local pad.
			if (unified_slot == 0 && command_index == 3)
			{
				if (!BeginPadPoll())
					return local_value;
			}

			if (!m_connected.load(std::memory_order_acquire))
				return local_value;

			if (unified_slot == 0)
			{
				m_capture.input[input_index] = local_value;
				return (m_role == Role::Host) ? m_local_output[input_index] : m_remote_output[input_index];
			}

			if (unified_slot == 1)
				return (m_role == Role::Host) ? m_remote_output[input_index] : m_local_output[input_index];

			return local_value;
		}

		void Stop()
		{
			m_running.store(false, std::memory_order_release);
			m_connected.store(false, std::memory_order_release);
			m_remote_cv.notify_all();

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

			if (m_receiver.joinable() && m_receiver.get_id() != std::this_thread::get_id())
				m_receiver.join();

			if (m_winsock_started)
			{
				WSACleanup();
				m_winsock_started = false;
			}
		}

	private:
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
				m_delay = static_cast<std::uint32_t>(std::clamp(std::atoi(delay), 1, 12));

			std::fprintf(stderr, "[ModernNetplay] configured: mode=%s port=%u delay=%u host=%s protocol=%u\n",
				(m_role == Role::Host) ? "host" : "client", static_cast<unsigned>(m_port),
				static_cast<unsigned>(m_delay), m_host.c_str(), static_cast<unsigned>(PROTOCOL_VERSION));
		}

		bool BeginPadPoll()
		{
			if (!EnsureConnected())
				return false;

			// Commit the pad sample captured by the preceding poll. In protocol v2 each
			// sample also carries PCSX2's canonical VSync frame number and a periodic
			// VM-memory hash. This lets us stop on the first real divergence instead of
			// silently pairing unrelated pad polls for minutes.
			if (m_have_capture)
			{
				m_local_frames[m_poll_frame] = m_capture;
				if (!SendFrame(m_poll_frame, m_capture))
				{
					Fail("failed to send controller frame");
					return false;
				}
				++m_poll_frame;
			}
			else
			{
				m_have_capture = true;
			}

			m_capture.input = NEUTRAL_FRAME;
			m_capture.emu_frame = static_cast<std::uint32_t>(g_FrameCount);
			m_capture.state_hash = 0;
			if (m_capture.emu_frame >= m_next_hash_emu_frame)
			{
				m_capture.state_hash = HashVmMemory();
				m_next_hash_emu_frame = m_capture.emu_frame + STATE_HASH_INTERVAL;
			}

			m_local_output = NEUTRAL_FRAME;
			m_remote_output = NEUTRAL_FRAME;

			if (m_poll_frame >= m_delay)
			{
				const std::uint32_t source_frame = m_poll_frame - m_delay;
				const auto local = m_local_frames.find(source_frame);
				if (local == m_local_frames.end())
				{
					Fail("local delayed frame is missing");
					return false;
				}

				InputSample remote;
				if (!WaitForRemoteFrame(source_frame, &remote))
				{
					Fail("timed out waiting for peer controller frame");
					return false;
				}

				if (local->second.emu_frame != remote.emu_frame)
				{
					std::fprintf(stderr,
						"[ModernNetplay] DESYNC: input timeline mismatch at poll=%u local-vsync=%u peer-vsync=%u\n",
						static_cast<unsigned>(source_frame), static_cast<unsigned>(local->second.emu_frame),
						static_cast<unsigned>(remote.emu_frame));
					Fail("emulation frame timeline diverged");
					return false;
				}

				if (local->second.state_hash != remote.state_hash &&
					(local->second.state_hash != 0 || remote.state_hash != 0))
				{
					std::fprintf(stderr,
						"[ModernNetplay] DESYNC: VM state mismatch at poll=%u vsync=%u local=%016llx peer=%016llx\n",
						static_cast<unsigned>(source_frame), static_cast<unsigned>(local->second.emu_frame),
						static_cast<unsigned long long>(local->second.state_hash),
						static_cast<unsigned long long>(remote.state_hash));
					Fail("virtual machine state hash diverged");
					return false;
				}

				m_local_output = local->second.input;
				m_remote_output = remote.input;
			}

			PruneFrames();
			return true;
		}

		bool EnsureConnected()
		{
			if (m_connected.load(std::memory_order_acquire))
				return true;
			if (m_failed.load(std::memory_order_acquire))
				return false;

			std::lock_guard<std::mutex> connect_lock(m_connect_mutex);
			if (m_connected.load(std::memory_order_acquire))
				return true;
			if (m_failed.load(std::memory_order_acquire))
				return false;

			WSADATA data{};
			if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
			{
				Fail("WSAStartup failed");
				return false;
			}
			m_winsock_started = true;

			SOCKET socket = (m_role == Role::Host) ? AcceptPeer() : ConnectToHost();
			if (socket == INVALID_SOCKET)
			{
				Fail("could not establish TCP connection");
				return false;
			}

			BOOL no_delay = TRUE;
			setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));

			if (!ExchangeHello(socket))
			{
				closesocket(socket);
				Fail("Netplay handshake failed");
				return false;
			}

			{
				std::lock_guard<std::mutex> socket_lock(m_socket_mutex);
				m_socket = socket;
			}
			m_running.store(true, std::memory_order_release);
			m_connected.store(true, std::memory_order_release);
			m_receiver = std::thread([this]() { ReceiverLoop(); });

			std::fprintf(stderr, "[ModernNetplay] peer connected; protocol=%u delay=%u\n",
				static_cast<unsigned>(PROTOCOL_VERSION), static_cast<unsigned>(m_delay));
			return true;
		}

		SOCKET AcceptPeer()
		{
			SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (listener == INVALID_SOCKET)
				return INVALID_SOCKET;

			BOOL reuse = TRUE;
			setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_ANY);
			address.sin_port = htons(m_port);

			if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
				listen(listener, 1) == SOCKET_ERROR)
			{
				closesocket(listener);
				return INVALID_SOCKET;
			}

			std::fprintf(stderr, "[ModernNetplay] hosting on TCP port %u; waiting for one peer...\n",
				static_cast<unsigned>(m_port));
			SOCKET peer = accept(listener, nullptr, nullptr);
			closesocket(listener);
			return peer;
		}

		SOCKET ConnectToHost()
		{
			addrinfo hints{};
			hints.ai_family = AF_UNSPEC;
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_protocol = IPPROTO_TCP;

			addrinfo* result = nullptr;
			const std::string port = std::to_string(m_port);
			if (getaddrinfo(m_host.c_str(), port.c_str(), &hints, &result) != 0)
				return INVALID_SOCKET;

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
			return connected;
		}

		bool ExchangeHello(SOCKET socket)
		{
			std::array<std::uint8_t, 16> outgoing{};
			WriteU32(outgoing.data() + 0, HELLO_MAGIC);
			WriteU32(outgoing.data() + 4, PROTOCOL_VERSION);
			WriteU32(outgoing.data() + 8, static_cast<std::uint32_t>(m_role));
			WriteU32(outgoing.data() + 12, m_delay);

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

			// Host is authoritative for delay so both virtual machines use the same queue depth.
			m_delay = std::clamp<std::uint32_t>(ReadU32(incoming.data() + 12), 1, 12);
			return true;
		}

		bool ValidateHello(const std::array<std::uint8_t, 16>& hello, Role expected_role) const
		{
			return ReadU32(hello.data() + 0) == HELLO_MAGIC && ReadU32(hello.data() + 4) == PROTOCOL_VERSION &&
				ReadU32(hello.data() + 8) == static_cast<std::uint32_t>(expected_role);
		}

		bool SendFrame(std::uint32_t frame, const InputSample& sample)
		{
			std::array<std::uint8_t, 26> packet{};
			WriteU32(packet.data() + 0, FRAME_MAGIC);
			WriteU32(packet.data() + 4, frame);
			WriteU32(packet.data() + 8, sample.emu_frame);
			WriteU64(packet.data() + 12, sample.state_hash);
			std::memcpy(packet.data() + 20, sample.input.data(), sample.input.size());

			std::lock_guard<std::mutex> lock(m_socket_mutex);
			return m_socket != INVALID_SOCKET && SendAll(m_socket, packet.data(), packet.size());
		}

		void ReceiverLoop()
		{
			while (m_running.load(std::memory_order_acquire))
			{
				std::array<std::uint8_t, 26> packet{};
				SOCKET socket;
				{
					std::lock_guard<std::mutex> lock(m_socket_mutex);
					socket = m_socket;
				}
				if (socket == INVALID_SOCKET || !ReceiveAll(socket, packet.data(), packet.size()))
					break;

				if (ReadU32(packet.data() + 0) != FRAME_MAGIC)
					break;

				InputSample sample;
				sample.emu_frame = ReadU32(packet.data() + 8);
				sample.state_hash = ReadU64(packet.data() + 12);
				std::memcpy(sample.input.data(), packet.data() + 20, sample.input.size());
				{
					std::lock_guard<std::mutex> lock(m_remote_mutex);
					m_remote_frames[ReadU32(packet.data() + 4)] = sample;
				}
				m_remote_cv.notify_all();
			}

			m_connected.store(false, std::memory_order_release);
			m_running.store(false, std::memory_order_release);
			m_remote_cv.notify_all();
			std::fprintf(stderr, "[ModernNetplay] peer disconnected\n");
		}

		bool WaitForRemoteFrame(std::uint32_t frame, InputSample* output)
		{
			std::unique_lock<std::mutex> lock(m_remote_mutex);
			const bool ready = m_remote_cv.wait_for(lock, std::chrono::seconds(RECEIVE_TIMEOUT_SECONDS), [this, frame]() {
				return m_remote_frames.find(frame) != m_remote_frames.end() ||
					!m_connected.load(std::memory_order_acquire);
			});
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
			if (m_poll_frame < 180)
				return;
			const std::uint32_t keep_from = m_poll_frame - 120;
			for (auto it = m_local_frames.begin(); it != m_local_frames.end();)
				it = (it->first < keep_from) ? m_local_frames.erase(it) : std::next(it);
			std::lock_guard<std::mutex> lock(m_remote_mutex);
			for (auto it = m_remote_frames.begin(); it != m_remote_frames.end();)
				it = (it->first < keep_from) ? m_remote_frames.erase(it) : std::next(it);
		}

		void Fail(const char* message)
		{
			if (!m_failed.exchange(true, std::memory_order_acq_rel))
				std::fprintf(stderr, "[ModernNetplay] ERROR: %s (WSA=%d)\n", message, WSAGetLastError());
			m_connected.store(false, std::memory_order_release);
			m_running.store(false, std::memory_order_release);
			m_remote_cv.notify_all();
		}

		Role m_role = Role::Disabled;
		std::string m_host = "127.0.0.1";
		std::uint16_t m_port = DEFAULT_PORT;
		std::uint32_t m_delay = 2;
		std::uint32_t m_poll_frame = 0;
		std::uint32_t m_next_hash_emu_frame = STATE_HASH_INTERVAL;
		bool m_have_capture = false;
		bool m_winsock_started = false;
		InputSample m_capture{};
		InputFrame m_local_output = NEUTRAL_FRAME;
		InputFrame m_remote_output = NEUTRAL_FRAME;
		std::unordered_map<std::uint32_t, InputSample> m_local_frames;
		std::unordered_map<std::uint32_t, InputSample> m_remote_frames;
		SOCKET m_socket = INVALID_SOCKET;
		std::atomic<bool> m_connected{false};
		std::atomic<bool> m_running{false};
		std::atomic<bool> m_failed{false};
		std::mutex m_connect_mutex;
		std::mutex m_socket_mutex;
		std::mutex m_remote_mutex;
		std::condition_variable m_remote_cv;
		std::thread m_receiver;
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

void ApplyDeterministicConfig()
{
	if (!GetSession().IsConfigured())
		return;

	// Classic PCSX2 Online did considerably more than exchange pad bytes: it forced
	// both emulators onto a conservative deterministic profile. v0.1 omitted that,
	// which allowed two otherwise-connected VMs to drift after several minutes.
	EmuConfig.HostFs = false;
	EmuConfig.EnablePatches = false;
	EmuConfig.EnableCheats = false;
	EmuConfig.EnableWideScreenPatches = false;
	EmuConfig.EnableNoInterlacingPatches = false;
	EmuConfig.CdvdVerboseReads = false;
	EmuConfig.CdvdDumpBlocks = false;
	EmuConfig.EnableGameFixes = true;
	EmuConfig.Pad.MultitapPort0_Enabled = false;
	EmuConfig.Pad.MultitapPort1_Enabled = false;
	EmuConfig.Speedhacks.DisableAll();
	EmuConfig.Cpu = Pcsx2Config::CpuOptions();
	EmuConfig.Profiler = Pcsx2Config::ProfilerOptions();
	EmuConfig.Trace.Enabled = false;
	EmuConfig.EmulationSpeed.SyncToHostRefreshRate = false;
	EmuConfig.EmulationSpeed.UseVSyncForTiming = false;
	EmuConfig.EmulationSpeed.NominalScalar = 1.0f;
	EmuConfig.GS.SynchronousMTGS = true;
	EmuConfig.GS.SkipDuplicateFrames = false;

	// Until host-to-client memory-card synchronization is ported, disable all cards
	// during Netplay. Different card contents can alter unlocks, settings and RNG state
	// before the first synchronized controller sample.
	for (Pcsx2Config::McdOptions& card : EmuConfig.Mcd)
		card.Enabled = false;

	// Do not seed games from two different host clocks.
	EmuConfig.ManuallySetRealTimeClock = true;
	EmuConfig.RtcYear = 0; // 2000 (PCSX2 stores year as an offset from 2000)
	EmuConfig.RtcMonth = 1;
	EmuConfig.RtcDay = 1;
	EmuConfig.RtcHour = 0;
	EmuConfig.RtcMinute = 0;
	EmuConfig.RtcSecond = 0;
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
bool IsConfigured()
{
	return false;
}

void ApplyDeterministicConfig()
{
}

std::uint8_t HandlePadResponse(std::uint8_t, std::uint32_t, std::uint8_t local_value)
{
	return local_value;
}

void Shutdown()
{
}
#endif
} // namespace ModernNetplay