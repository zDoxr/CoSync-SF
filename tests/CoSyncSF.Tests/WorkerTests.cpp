// Exercises the plugin's threading model without the game: a host worker
// (in-process server + local client) and a client worker on separate threads,
// with this thread acting as each "game thread".

#include "cosync/net/NetworkWorker.h"
#include "cosync/shared/GameRuntime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <thread>

using namespace cosync;
using namespace cosync::net;

namespace
{
    template <class Pred>
    bool WaitFor(Pred pred, int timeoutMs = 8000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
}

TEST(NetworkWorker, HostAndClientSyncAcrossThreads)
{
    constexpr std::uint16_t kPort = 47820;

    WorkerConfig hostCfg;
    hostCfg.mode = WorkerMode::Host;
    hostCfg.listenPort = kPort;
    hostCfg.server.requiredGameRuntime = game::kTargetRuntime;
    hostCfg.client.playerName = "Host";
    hostCfg.client.gameRuntime = game::kTargetRuntime;
    NetworkWorker host(hostCfg);
    std::string error;
    ASSERT_TRUE(host.Start(error)) << error;

    WorkerConfig clientCfg;
    clientCfg.mode = WorkerMode::Client;
    clientCfg.connectTo = { "127.0.0.1", kPort };
    clientCfg.client.playerName = "Guest";
    clientCfg.client.gameRuntime = game::kTargetRuntime;
    NetworkWorker guest(clientCfg);
    ASSERT_TRUE(guest.Start(error)) << error;

    ASSERT_TRUE(WaitFor([&] { return host.Status().peers == 1 && guest.Status().peers == 1; }))
        << "host=" << ToString(host.Status().state) << " guest=" << ToString(guest.Status().state);
    EXPECT_EQ(host.Roster().begin()->second, "Guest");
    EXPECT_EQ(guest.Roster().begin()->second, "Host");

    // "Game thread" submits a transform on the guest; host's game thread reads it.
    std::vector<proto::PlayerTransform> received;
    ASSERT_TRUE(WaitFor([&] {
        guest.SubmitLocalTransform(42, { 1, 2, 3 }, { 0, 0, 0 });
        host.TakeRemoteTransforms(received);
        return !received.empty();
    }));
    EXPECT_EQ(received.back().locationId, 42u);
    EXPECT_EQ(received.back().playerId, guest.Status().localPlayer);

    // Ordered session events were queued for the game thread.
    std::vector<ClientEvent> events;
    host.DrainEvents(events, 1000);
    EXPECT_TRUE(std::ranges::any_of(events, [](const ClientEvent& e) {
        return std::holds_alternative<client_event::PeerJoined>(e);
    }));
    EXPECT_FALSE(host.ConsumeOverflow());

    guest.Stop();
    ASSERT_TRUE(WaitFor([&] { return host.Status().peers == 0; }));
    host.Stop();
    EXPECT_FALSE(host.Running());
}

TEST(NetworkWorker, StartFailsCleanlyWhenPortInUse)
{
    constexpr std::uint16_t kPort = 47821;
    WorkerConfig cfg;
    cfg.mode = WorkerMode::Host;
    cfg.listenPort = kPort;
    cfg.client.playerName = "First";
    NetworkWorker first(cfg);
    std::string error;
    ASSERT_TRUE(first.Start(error)) << error;

    cfg.client.playerName = "Second";
    NetworkWorker second(cfg);
    EXPECT_FALSE(second.Start(error));
    EXPECT_FALSE(error.empty());
    first.Stop();
}

TEST(NetworkWorker, EventOverflowIsReportedNotUnbounded)
{
    constexpr std::uint16_t kPort = 47822;
    WorkerConfig cfg;
    cfg.mode = WorkerMode::Host;
    cfg.listenPort = kPort;
    cfg.client.playerName = "Host";
    cfg.eventQueueCapacity = 1;  // game thread "stalled": never drains
    NetworkWorker host(cfg);
    std::string error;
    ASSERT_TRUE(host.Start(error)) << error;
    ASSERT_TRUE(WaitFor([&] { return host.Status().state == ClientState::InSession; }));
    EXPECT_TRUE(WaitFor([&] { return host.Status().droppedEvents > 0; }));
    EXPECT_TRUE(host.ConsumeOverflow());
    EXPECT_FALSE(host.ConsumeOverflow());  // reported once
    host.Stop();
}
