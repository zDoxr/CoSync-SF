#include "cosync/net/LanDiscovery.h"
#include "cosync/shared/Protocol.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using namespace cosync;
using namespace cosync::net::discovery;

TEST(Discovery, QueryIsPaddedAndRoundTrips)
{
    const auto q = EncodeQuery(0xA1B2C3D4);
    EXPECT_EQ(q.size(), kPacketBytes);
    EXPECT_EQ(DecodeQuery(q), 0xA1B2C3D4u);

    auto shortQ = q;
    shortQ.resize(10);  // unpadded queries are ignored (anti-amplification)
    EXPECT_FALSE(DecodeQuery(shortQ));
    auto badMagic = q;
    badMagic[0] ^= 1;
    EXPECT_FALSE(DecodeQuery(badMagic));
}

TEST(Discovery, ResponseNeverExceedsQuerySize)
{
    ServerInfo info;
    info.gamePort = 37015;
    info.serverName = std::string(proto::kMaxServerNameBytes, 'x');
    const auto r = EncodeResponse(1, info);
    EXPECT_LE(r.size(), kPacketBytes);

    const auto decoded = DecodeResponse(r);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->first, 1u);
    EXPECT_EQ(decoded->second.serverName, info.serverName);

    auto trailing = r;
    trailing.push_back(0);
    EXPECT_FALSE(DecodeResponse(trailing));
}

TEST(Discovery, ResponderAnswersUnicastQueryOnLoopback)
{
    constexpr std::uint16_t kTestPort = 47816;
    Responder responder;
    std::string error;
    ASSERT_TRUE(responder.Start(kTestPort, error)) << error;

    ServerInfo info;
    info.protocolMajor = proto::kProtocolMajor;
    info.gamePort = 47815;
    info.players = 1;
    info.maxPlayers = 4;
    info.passwordRequired = true;
    info.serverName = "Loopback Test";

    std::atomic<bool> stop{ false };
    std::thread pump([&] {
        while (!stop) {
            responder.Poll(info);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    const std::vector<std::string> hosts{ "127.0.0.1" };
    const auto found = Discover(kTestPort, 500, false, hosts, error);
    stop = true;
    pump.join();

    ASSERT_EQ(found.size(), 1u) << error;
    EXPECT_EQ(found[0].endpoint.host, "127.0.0.1");
    EXPECT_EQ(found[0].endpoint.port, 47815);
    EXPECT_EQ(found[0].info.serverName, "Loopback Test");
    EXPECT_TRUE(found[0].info.passwordRequired);
}
