// End-to-end tests over real UDP sockets on 127.0.0.1 using GameNetworkingSockets.

#include "cosync/net/GnsTransport.h"
#include "cosync/net/SessionClient.h"
#include "cosync/net/SessionServer.h"
#include "cosync/shared/GameRuntime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

using namespace cosync;
using namespace cosync::net;

namespace
{
    // Pump `step` until `done` or the deadline passes.
    bool PumpUntil(const std::function<void()>& step, const std::function<bool()>& done, int timeoutMs = 8000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            step();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    template <class T>
    std::size_t Count(const std::vector<ClientEvent>& events)
    {
        return static_cast<std::size_t>(std::ranges::count_if(events, [](const ClientEvent& e) { return std::holds_alternative<T>(e); }));
    }
}

TEST(GnsIntegration, TwoClientsJoinRelayTransformsAndLeave)
{
    constexpr std::uint16_t kPort = 47815;
    GnsTransport serverTransport;
    ASSERT_TRUE(serverTransport.IsReady()) << serverTransport.InitError();
    ServerConfig scfg;
    scfg.serverName = "GNS Test";
    SessionServer server(serverTransport, scfg);
    std::string error;
    ASSERT_TRUE(server.Start(kPort, error)) << error;

    GnsTransport ta, tb;
    ClientConfig ca, cb;
    ca.playerName = "Alice";
    cb.playerName = "Bob";
    SessionClient a(ta, ca), b(tb, cb);
    ASSERT_TRUE(a.Connect({ "127.0.0.1", kPort }, error)) << error;
    ASSERT_TRUE(b.Connect({ "127.0.0.1", kPort }, error)) << error;

    std::vector<ClientEvent> ea, eb;
    auto step = [&] {
        server.Update();
        a.Update(ea);
        b.Update(eb);
    };

    ASSERT_TRUE(PumpUntil(step, [&] {
        return a.State() == ClientState::InSession && b.State() == ClientState::InSession && a.Roster().size() == 1 &&
               b.Roster().size() == 1;
    })) << "a=" << ToString(a.State()) << " b=" << ToString(b.State());
    EXPECT_NE(a.LocalPlayerId(), b.LocalPlayerId());
    EXPECT_EQ(a.CurrentSessionId(), server.Id());

    // Unreliable transforms: send several, expect at least one to arrive.
    eb.clear();
    ASSERT_TRUE(PumpUntil(
        [&] {
            a.SendTransform(1, { 10, 20, 30 }, { 0, 0, 0 });
            step();
        },
        [&] { return Count<client_event::RemoteTransform>(eb) > 0; }));
    for (const auto& e : eb) {
        if (auto* t = std::get_if<client_event::RemoteTransform>(&e)) {
            EXPECT_EQ(t->transform.playerId, a.LocalPlayerId());
            EXPECT_FLOAT_EQ(t->transform.position.y, 20.0f);
        }
    }

    // Heartbeat RTT measured over the real socket.
    ASSERT_TRUE(PumpUntil(step, [&] { return Count<client_event::RttSample>(ea) > 0; }, 4000));

    // Graceful leave is seen by the other player as Quit.
    eb.clear();
    a.Disconnect();
    ASSERT_TRUE(PumpUntil(step, [&] { return b.Roster().empty(); }));
    bool sawQuit = false;
    for (const auto& e : eb) {
        if (auto* l = std::get_if<client_event::PeerLeft>(&e)) sawQuit = l->reason == proto::LeaveReason::Quit;
    }
    EXPECT_TRUE(sawQuit);
    EXPECT_EQ(server.ConnectedPlayerCount(), 1u);

    // Server shutdown reaches the remaining client.
    server.Stop();
    ASSERT_TRUE(PumpUntil(step, [&] { return b.State() == ClientState::Disconnected; }));
}

TEST(GnsIntegration, RejectIsDeliveredBeforeClose)
{
    constexpr std::uint16_t kPort = 47818;
    GnsTransport serverTransport;
    ServerConfig scfg;
    scfg.requiredGameRuntime = game::kTargetRuntime;
    SessionServer server(serverTransport, scfg);
    std::string error;
    ASSERT_TRUE(server.Start(kPort, error)) << error;

    GnsTransport tc;
    ClientConfig cc;
    cc.playerName = "OldBuild";
    cc.gameRuntime = game::MakeRuntimeVersion(1, 15, 222);
    SessionClient client(tc, cc);
    ASSERT_TRUE(client.Connect({ "127.0.0.1", kPort }, error));

    std::vector<ClientEvent> events;
    ASSERT_TRUE(PumpUntil(
        [&] {
            server.Update();
            client.Update(events);
        },
        [&] { return client.State() == ClientState::Disconnected; }));
    ASSERT_EQ(Count<client_event::Rejected>(events), 1u);
    for (const auto& e : events) {
        if (auto* r = std::get_if<client_event::Rejected>(&e)) {
            EXPECT_EQ(r->reject.reason, proto::RejectReason::GameVersionMismatch);
            EXPECT_NE(r->reject.detail.find("1.16.244"), std::string::npos);
        }
    }
}

TEST(GnsIntegration, ConnectingToClosedPortFails)
{
    GnsOptions opts;
    opts.connectTimeoutMs = 1500;
    GnsTransport tc(opts);
    ClientConfig cc;
    cc.playerName = "Nobody";
    cc.connectTimeoutMs = 3000;
    SessionClient client(tc, cc);
    std::string error;
    ASSERT_TRUE(client.Connect({ "127.0.0.1", 47899 }, error));
    std::vector<ClientEvent> events;
    ASSERT_TRUE(PumpUntil([&] { client.Update(events); },
                          [&] { return client.State() == ClientState::Disconnected; }, 6000));
}

TEST(GnsIntegration, OversizeMessageIsRefusedByTransport)
{
    constexpr std::uint16_t kPort = 47819;
    GnsTransport serverTransport;
    std::string error;
    ASSERT_TRUE(serverTransport.Listen(kPort, error)) << error;

    GnsTransport raw;
    const auto conn = raw.Connect({ "127.0.0.1", kPort }, error);
    ASSERT_NE(conn, kInvalidConnection) << error;

    std::vector<TransportEvent> sev, cev;
    auto step = [&] {
        serverTransport.Poll(sev);
        raw.Poll(cev);
    };
    ASSERT_TRUE(PumpUntil(step, [&] {
        return std::ranges::any_of(cev, [](auto& e) { return e.type == TransportEventType::Connected; });
    }));

    std::vector<std::uint8_t> huge(proto::kMaxMessageBytes * 2, 0xAB);
    raw.Send(conn, huge, Delivery::Reliable);
    ASSERT_TRUE(PumpUntil(step, [&] {
        return std::ranges::any_of(cev, [](auto& e) { return e.type == TransportEventType::Disconnected; });
    }));
    EXPECT_FALSE(std::ranges::any_of(sev, [](auto& e) { return e.type == TransportEventType::Message; }));
}
