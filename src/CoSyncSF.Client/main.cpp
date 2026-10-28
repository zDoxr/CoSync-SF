// cosync-testclient: headless protocol client for exercising a CoSync-SF
// session over a real network without Starfield. It speaks the same protocol
// as the SFSE plugin and can send synthetic transforms (a circle path).

#include "cosync/net/GnsTransport.h"
#include "cosync/net/LanDiscovery.h"
#include "cosync/net/SessionClient.h"
#include "cosync/shared/GameRuntime.h"
#include "cosync/shared/Log.h"

#include <Windows.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

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
        std::string connect;
        bool discover = false;
        std::vector<std::string> discoverHosts;
        std::uint16_t discoveryPort = net::discovery::kDefaultPort;
        bool sendTransforms = false;
        std::uint32_t durationSeconds = 0;
        int expectPeers = -1;
        std::uint32_t expectTransforms = 0;
        net::ClientConfig client;
    };

    void PrintUsage()
    {
        std::fputs(
            "usage: cosync-testclient (--connect HOST:PORT | --discover) [options]\n"
            "  --connect HOST:PORT    numeric address of a session (port defaults to 37015)\n"
            "  --discover             list LAN sessions (broadcast) and exit, or join the first with --join-first\n"
            "  --discover-host IP     also query this address directly (repeatable)\n"
            "  --discovery-port N     default 37016\n"
            "  --join-first           with --discover, connect to the first session found\n"
            "  --name NAME            player name (required to join)\n"
            "  --password TEXT        session password (or set COSYNC_PASSWORD)\n"
            "  --runtime V            claim Starfield runtime V (e.g. 1.16.244); default none\n"
            "  --send-transforms      send a synthetic moving transform at 20 Hz\n"
            "  --duration SECONDS     disconnect gracefully after this long\n"
            "  --expect-peers N       exit 0 once N other players are in the roster\n"
            "  --expect-transforms N  also require N remote transforms before exiting 0\n"
            "  --no-reconnect         do not resume after a dropped connection\n"
            "  --log-level LEVEL      debug | info | warn | error\n",
            stderr);
    }

    bool ParseU32(std::string_view s, std::uint32_t& out)
    {
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
        return ec == std::errc{} && p == s.data() + s.size();
    }

    bool ParseArgs(int argc, char** argv, Options& o, bool& joinFirst)
    {
        if (const char* pw = std::getenv("COSYNC_PASSWORD")) {
            o.client.password = pw;
        }
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            auto value = [&]() -> std::string_view { return i + 1 < argc ? argv[++i] : std::string_view{}; };
            bool ok = true;
            std::uint32_t n = 0;
            if (arg == "--connect") o.connect = value();
            else if (arg == "--discover") o.discover = true;
            else if (arg == "--discover-host") o.discoverHosts.emplace_back(value());
            else if (arg == "--discovery-port") { ok = ParseU32(value(), n) && n > 0 && n < 65536; o.discoveryPort = static_cast<std::uint16_t>(n); }
            else if (arg == "--join-first") joinFirst = true;
            else if (arg == "--name") o.client.playerName = value();
            else if (arg == "--password") o.client.password = value();
            else if (arg == "--runtime") {
                auto rt = game::ParseRuntimeVersion(value());
                ok = rt.has_value();
                if (rt) o.client.gameRuntime = *rt;
            }
            else if (arg == "--send-transforms") o.sendTransforms = true;
            else if (arg == "--duration") ok = ParseU32(value(), o.durationSeconds);
            else if (arg == "--expect-peers") { ok = ParseU32(value(), n) && n <= 64; o.expectPeers = static_cast<int>(n); }
            else if (arg == "--expect-transforms") ok = ParseU32(value(), o.expectTransforms);
            else if (arg == "--no-reconnect") o.client.autoReconnect = false;
            else if (arg == "--log-level") {
                const auto v = value();
                if (v == "debug") log::SetMinLevel(log::Level::Debug);
                else if (v == "info") log::SetMinLevel(log::Level::Info);
                else if (v == "warn") log::SetMinLevel(log::Level::Warn);
                else if (v == "error") log::SetMinLevel(log::Level::Error);
                else ok = false;
            } else {
                ok = false;
            }
            if (!ok) {
                std::fprintf(stderr, "invalid or incomplete argument: %s\n", argv[i]);
                return false;
            }
        }
        return o.discover || !o.connect.empty();
    }

    void PrintEvent(const net::ClientEvent& ev)
    {
        std::visit(
            [](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                namespace ce = net::client_event;
                if constexpr (std::is_same_v<T, ce::Welcomed>) {
                    log::Info("welcomed to '{}' as player {} (session {:016x}{})", e.welcome.serverName,
                              e.welcome.playerId, e.welcome.sessionId, e.welcome.resumed ? ", resumed" : "");
                } else if constexpr (std::is_same_v<T, ce::Rejected>) {
                    log::Warn("rejected: {} - {}", proto::ToString(e.reject.reason), e.reject.detail);
                } else if constexpr (std::is_same_v<T, ce::PeerJoined>) {
                    log::Info("peer joined: {} '{}'", e.player.playerId, e.player.name);
                } else if constexpr (std::is_same_v<T, ce::PeerLeft>) {
                    log::Info("peer left: {} ({})", e.playerId, proto::ToString(e.reason));
                } else if constexpr (std::is_same_v<T, ce::RemoteTransform>) {
                    log::Debug("transform from {} #{}: ({:.1f}, {:.1f}, {:.1f})", e.transform.playerId,
                               e.transform.sequence, e.transform.position.x, e.transform.position.y,
                               e.transform.position.z);
                } else if constexpr (std::is_same_v<T, ce::RttSample>) {
                    log::Debug("rtt {} ms", e.rttMs);
                }
                // StateChanged is already logged by SessionClient.
            },
            ev);
    }
}

int main(int argc, char** argv)
{
    Options options;
    bool joinFirst = false;
    if (!ParseArgs(argc, argv, options, joinFirst)) {
        PrintUsage();
        return 2;
    }
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

    if (options.discover) {
        std::string error;
        const auto servers = net::discovery::Discover(options.discoveryPort, 1500, true, options.discoverHosts, error);
        if (!error.empty()) {
            log::Error("discovery failed: {}", error);
            return 1;
        }
        for (const auto& s : servers) {
            std::printf("%s  '%s'  %u/%u players  protocol %u.%u  runtime %s%s  (%u ms)\n",
                        s.endpoint.ToString().c_str(), s.info.serverName.c_str(), s.info.players, s.info.maxPlayers,
                        s.info.protocolMajor, s.info.protocolMinor,
                        game::FormatRuntimeVersion(s.info.requiredGameRuntime).c_str(),
                        s.info.passwordRequired ? "  [password]" : "", s.latencyMs);
        }
        if (servers.empty()) {
            std::puts("no sessions found");
            return joinFirst ? 1 : 0;
        }
        if (!joinFirst) {
            return 0;
        }
        options.connect = servers.front().endpoint.ToString();
    }

    const auto endpoint = net::Endpoint::Parse(options.connect, 37015);
    if (!endpoint) {
        log::Error("'{}' is not a valid HOST:PORT", options.connect);
        return 2;
    }
    if (!proto::IsValidPlayerName(options.client.playerName)) {
        log::Error("--name is required (1-32 printable UTF-8 bytes, no leading/trailing spaces)");
        return 2;
    }

    net::GnsTransport transport;
    if (!transport.IsReady()) {
        log::Error("networking unavailable: {}", transport.InitError());
        return 1;
    }
    net::SessionClient client(transport, options.client);
    std::string error;
    if (!client.Connect(*endpoint, error)) {
        log::Error("connect failed: {}", error);
        return 1;
    }

    const auto started = net::SteadyNowMs();
    auto nextTransform = started;
    std::uint32_t remoteTransforms = 0;
    int exitCode = 0;
    bool done = false;
    std::vector<net::ClientEvent> events;

    while (!g_stop && !done) {
        events.clear();
        client.Update(events);
        for (const auto& ev : events) {
            PrintEvent(ev);
            if (std::holds_alternative<net::client_event::RemoteTransform>(ev)) {
                ++remoteTransforms;
            }
        }

        const auto now = net::SteadyNowMs();
        if (client.State() == net::ClientState::Disconnected) {
            exitCode = 1;
            done = true;
        } else if (client.State() == net::ClientState::InSession) {
            if (options.sendTransforms && now >= nextTransform) {
                const float t = static_cast<float>(now - started) / 1000.0f;
                client.SendTransform(1, { 100.0f * std::cos(t), 100.0f * std::sin(t), 0.0f }, { 0.0f, 0.0f, t });
                nextTransform = now + 50;
            }
            if (options.expectPeers >= 0 && static_cast<int>(client.Roster().size()) >= options.expectPeers &&
                remoteTransforms >= options.expectTransforms) {
                log::Info("expectation met: {} peers, {} remote transforms", client.Roster().size(), remoteTransforms);
                done = true;
            }
        }
        if (options.durationSeconds != 0 && now - started >= options.durationSeconds * 1000ull) {
            if (options.expectPeers >= 0) {
                log::Error("expectation not met before --duration elapsed ({} peers, {} transforms)",
                           client.Roster().size(), remoteTransforms);
                exitCode = 1;
            }
            done = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    client.Disconnect();
    for (int i = 0; i < 10; ++i) {  // flush the graceful Disconnect
        events.clear();
        client.Update(events);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return exitCode;
}
