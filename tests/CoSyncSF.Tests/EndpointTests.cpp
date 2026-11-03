#include "cosync/net/Transport.h"

#include <gtest/gtest.h>

using cosync::net::Endpoint;

TEST(Endpoint, ParsesIpv4AndIpv6)
{
    auto a = Endpoint::Parse("192.168.1.20:37015");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->host, "192.168.1.20");
    EXPECT_EQ(a->port, 37015);
    EXPECT_EQ(a->ToString(), "192.168.1.20:37015");

    auto b = Endpoint::Parse("[::1]:5000");
    ASSERT_TRUE(b);
    EXPECT_EQ(b->host, "::1");
    EXPECT_EQ(b->ToString(), "[::1]:5000");

    auto c = Endpoint::Parse("10.0.0.1", 37015);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->port, 37015);

    auto d = Endpoint::Parse("fe80::1", 1234);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->host, "fe80::1");
}

TEST(Endpoint, RejectsMalformed)
{
    EXPECT_FALSE(Endpoint::Parse(""));
    EXPECT_FALSE(Endpoint::Parse("1.2.3.4:0"));
    EXPECT_FALSE(Endpoint::Parse("1.2.3.4:70000"));
    EXPECT_FALSE(Endpoint::Parse("1.2.3.4:12ab"));
    EXPECT_FALSE(Endpoint::Parse(":5000"));
    EXPECT_FALSE(Endpoint::Parse("[::1"));
    EXPECT_FALSE(Endpoint::Parse("1.2.3.4"));  // no port and no default
}

TEST(Endpoint, ParsesSteamPeers)
{
    const auto ep = Endpoint::Parse("steam:76561198000000001", 37015);
    ASSERT_TRUE(ep.has_value());
    EXPECT_EQ(ep->host, "steam:76561198000000001");
    EXPECT_EQ(ep->port, 0);  // Steam virtual port, not the UDP default
    EXPECT_EQ(ep->ToString(), "steam:76561198000000001");
    EXPECT_EQ(Endpoint::Parse(ep->ToString())->host, ep->host);
    EXPECT_FALSE(Endpoint::Parse("steam:abc").has_value());
    EXPECT_FALSE(Endpoint::Parse("steam:0").has_value());
    EXPECT_FALSE(Endpoint::Parse("steam:").has_value());
}
