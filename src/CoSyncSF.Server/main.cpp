// cosync-server: standalone CoSync-SF session server.
//
// This process manages sessions (connections, handshake, player registration,
// heartbeats, resume, transform relay, LAN discovery). It does NOT run the
// Starfield simulation; the game state lives in the connected game clients.
// See docs/RESEARCH.md ("Dedicated-server feasibility").

#include "cosync/net/GnsTransport.h"
#include "cosync/net/LanDiscovery.h"
#include "cosync/net/SessionServer.h"
#include "cosync/shared/GameRuntime.h"
#include "cosync/shared/Log.h"

#include <Windows.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

using namespace cosync;

namespace
{
    std::atomic<bool> g_stop{ false };

    BOOL WINAPI OnConsoleCtrl(DWORD)
    {
        g_stop = true;
        return TRUE;
    }

    struct Options
    {
        std::uint16_t port = 37015;
        std::uint16_t discoveryPort = net::discovery::kDefaultPort;
        std::uint32_t tickRate = 30;
        std::uint32_t runForSeconds = 0;
        net::ServerConfig server;
    };

    void PrintUsage()
    {
        std::fputs(
            "usage: cosync-server [options]\n"
            "  --port N               UDP game port (default 37015)\n"
            "  --discovery-port N     LAN discovery UDP port, 0 disables (default 37016)\n"
            "  --name TEXT            session name shown to players\n"
            "  --max-players N        1-64 (default 8)\n"
            "  --password TEXT        session password (or set COSYNC_PASSWORD)\n"
            "  --require-runtime V    require Starfield runtime V (e.g. 1.16.244) or 'any' (default any)\n"
            "  --resume-window MS     how long a dropped player's slot is held (default 30000)\n"
            "  --tick-rate HZ         update rate (default 30)\n"
            "  --run-for SECONDS      exit after this long (for scripted tests)\n"
            "  --log-level LEVEL      debug | info | warn | error (default info)\n",
            stderr);
    }

    template <class T>
    bool ParseNumber(std::string_view s, T& out, T min, T max)
    {
        unsigned long long v = 0;
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc{} || p != s.data() + s.size() || v < min || v > max) {
            return false;
        }
        out = static_cast<T>(v);
        return true;
    }

    bool ParseArgs(int argc, char** argv, Options& o)
    {
        if (const char* pw = std::getenv("COSYNC_PASSWORD")) {
            o.server.password = pw;
        }
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            auto value = [&]() -> std::string_view { return i + 1 < argc ? argv[++i] : std::string_view{}; };
            bool ok = true;
            if (arg == "--port") ok = ParseNumber<std::uint16_t>(value(), o.port, 1, 65535);
            else if (arg == "--discovery-port") ok = ParseNumber<std::uint16_t>(value(), o.discoveryPort, 0, 65535);
            else if (arg == "--name") o.server.serverName = value();
            else if (arg == "--max-players") ok = ParseNumber<std::uint16_t>(value(), o.server.maxPlayers, 1, proto::kMaxPlayersLimit);
            else if (arg == "--password") o.server.password = value();
            else if (arg == "--resume-window") ok = ParseNumber<std::uint32_t>(value(), o.server.resumeWindowMs, 0, 600000);
            else if (arg == "--tick-rate") ok = ParseNumber<std::uint32_t>(value(), o.tickRate, 1, 240);
            else if (arg == "--run-for") ok = ParseNumber<std::uint32_t>(value(), o.runForSeconds, 0, 86400);
            else if (arg == "--require-runtime") {
                const auto v = value();
                if (v == "any") {
                    o.server.requiredGameRuntime = 0;
                } else if (auto rt = game::ParseRuntimeVersion(v)) {
                    o.server.requiredGameRuntime = *rt;
                } else {
                    ok = false;
                }
            } else if (arg == "--log-level") {
                const auto v = value();
                if (v == "debug") log::SetMinLevel(log::Level::Debug);
                else if (v == "info") log::SetMinLevel(log::Level::Info);
                else if (v == "warn") log::SetMinLevel(log::Level::Warn);
                else if (v == "error") log::SetMinLevel(log::Level::Error);
                else ok = false;
            } else if (arg == "--help" || arg == "-h") {
                return false;
            } else {
                ok = false;
            }
            if (!ok) {
                std::fprintf(stderr, "invalid or incomplete argument: %s\n", argv[i]);
                return false;
            }
        }
        if (!proto::IsValidText(o.server.serverName, proto::kMaxServerNameBytes) || o.server.serverName.empty()) {
            std::fputs("--name must be 1-64 printable UTF-8 bytes\n", stderr);
            return false;
        }
        if (o.server.password.size() > proto::kMaxPasswordBytes) {
            std::fputs("password is limited to 64 bytes\n", stderr);
            return false;
        }
        return true;
    }
}

int main(int argc, char** argv)
{
    Options options;
    if (!ParseArgs(argc, argv, options)) {
        PrintUsage();
        return 2;
    }
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

    net::GnsTransport transport;
    if (!transport.IsReady()) {
        log::Error("networking unavailable: {}", transport.InitError());
        return 1;
    }

    net::SessionServer server(transport, options.server);
    std::string error;
    if (!server.Start(options.port, error)) {
        log::Error("failed to start: {}", error);
        return 1;
    }

    net::discovery::Responder discovery;
    if (options.discoveryPort != 0 && !discovery.Start(options.discoveryPort, error)) {
        log::Warn("LAN discovery disabled: {}", error);
    }

    const auto tick = std::chrono::microseconds(1'000'000 / options.tickRate);
    const auto started = net::SteadyNowMs();
    auto nextStatus = started + 30'000;
    auto nextTick = std::chrono::steady_clock::now();

    while (!g_stop) {
        server.Update();

        net::discovery::ServerInfo info;
        info.protocolMajor = proto::kProtocolMajor;
        info.protocolMinor = proto::kProtocolMinor;
        info.gamePort = options.port;
        info.players = static_cast<std::uint16_t>(server.ConnectedPlayerCount());
        info.maxPlayers = options.server.maxPlayers;
        info.requiredGameRuntime = options.server.requiredGameRuntime;
        info.passwordRequired = !options.server.password.empty();
        info.serverName = options.server.serverName;
        discovery.Poll(info);

        const auto now = net::SteadyNowMs();
        if (now >= nextStatus) {
            const auto& st = server.Stats();
            log::Info("status: {} connected, {} pending, {} msgs, {} relayed, {} violations, {} rejections",
                      server.ConnectedPlayerCount(), server.PendingConnectionCount(), st.messagesReceived,
                      st.transformsRelayed, st.protocolViolations, st.rejections);
            nextStatus = now + 30'000;
        }
        if (options.runForSeconds != 0 && now - started >= options.runForSeconds * 1000ull) {
            break;
        }

        nextTick += tick;
        std::this_thread::sleep_until(nextTick);
    }

    log::Info("shutting down");
    server.Stop();
    // Keep pumping briefly so lingering connections flush their Disconnect.
    for (int i = 0; i < 10; ++i) {
        server.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    discovery.Stop();
    transport.Shutdown();
    return 0;
}
