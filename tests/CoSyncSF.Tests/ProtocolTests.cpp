#include "cosync/shared/ByteStream.h"
#include "cosync/shared/GameRuntime.h"
#include "cosync/shared/Protocol.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace cosync;
using namespace cosync::proto;

namespace
{
    ResumeToken Token(std::uint8_t seed)
    {
        ResumeToken t;
        for (std::size_t i = 0; i < t.bytes.size(); ++i) t.bytes[i] = static_cast<std::uint8_t>(seed + i);
        return t;
    }

    std::vector<Message> SampleMessages()
    {
        Hello hello;
        hello.gameRuntime = game::kTargetRuntime;
        hello.clientBuild = "test/1";
        hello.playerName = "Väinö";  // multi-byte UTF-8
        hello.password = "secret";
        hello.resumeToken = Token(7);

        Welcome welcome;
        welcome.playerId = 3;
        welcome.sessionId = 0x0123456789ABCDEFull;
        welcome.resumeToken = Token(9);
        welcome.serverName = "Server";
        welcome.maxPlayers = 8;
        welcome.heartbeatIntervalMs = 1000;
        welcome.resumed = true;

        PlayerTransform t;
        t.playerId = 4;
        t.sequence = 99;
        t.locationId = 0x1234;
        t.position = { 1.5f, -2.5f, 3.25f };
        t.rotation = { 0.0f, 0.5f, -3.0f };

        return {
            hello,
            welcome,
            Reject{ RejectReason::NameInUse, 1, 0, "taken" },
            SessionSnapshot{ { { 1, "Alice" }, { 2, "Bob" } } },
            PlayerJoined{ { 5, "Carol" } },
            PlayerLeft{ 5, LeaveReason::ConnectionLost },
            Heartbeat{ 42, 123456789 },
            HeartbeatAck{ 42, 123456789 },
            Disconnect{ DisconnectReason::Quit, "bye" },
            t,
        };
    }

    std::vector<std::uint8_t> WithHeader(std::uint16_t type, std::uint16_t flags, std::vector<std::uint8_t> payload)
    {
        ByteWriter w;
        w.U16(type);
        w.U16(flags);
        w.Bytes(payload);
        return w.Take();
    }
}

TEST(Protocol, RoundTripsEveryMessageType)
{
    for (const auto& msg : SampleMessages()) {
        const auto bytes = Encode(msg);
        ASSERT_LE(bytes.size(), kMaxMessageBytes);
        const auto decoded = Decode(bytes);
        ASSERT_TRUE(decoded.Ok()) << ToString(TypeOf(msg)) << ": " << ToString(decoded.error);
        EXPECT_EQ(TypeOf(*decoded.message), TypeOf(msg));
        // Re-encoding must be byte-identical (no lossy fields).
        EXPECT_EQ(Encode(*decoded.message), bytes) << ToString(TypeOf(msg));
    }
}

TEST(Protocol, HelloFieldsSurviveRoundTrip)
{
    const auto decoded = Decode(Encode(SampleMessages()[0]));
    ASSERT_TRUE(decoded.Ok());
    const auto& h = std::get<Hello>(*decoded.message);
    EXPECT_EQ(h.magic, kMagic);
    EXPECT_EQ(h.gameRuntime, game::kTargetRuntime);
    EXPECT_EQ(h.playerName, "Väinö");
    EXPECT_EQ(h.password, "secret");
    EXPECT_TRUE(h.resumeToken.ConstantTimeEquals(Token(7)));
}

TEST(Protocol, EveryTruncationIsRejected)
{
    for (const auto& msg : SampleMessages()) {
        const auto bytes = Encode(msg);
        for (std::size_t len = 0; len < bytes.size(); ++len) {
            const auto r = Decode(std::span(bytes.data(), len));
            EXPECT_FALSE(r.Ok()) << ToString(TypeOf(msg)) << " accepted at length " << len;
        }
    }
}

TEST(Protocol, RejectsTrailingBytes)
{
    for (const auto& msg : SampleMessages()) {
        auto bytes = Encode(msg);
        bytes.push_back(0);
        const auto r = Decode(bytes);
        EXPECT_FALSE(r.Ok()) << ToString(TypeOf(msg));
        EXPECT_EQ(r.error, DecodeError::TrailingBytes) << ToString(TypeOf(msg));
    }
}

TEST(Protocol, RejectsEmptyOversizeUnknownAndFlags)
{
    EXPECT_EQ(Decode({}).error, DecodeError::Empty);

    std::vector<std::uint8_t> huge(kMaxMessageBytes + 1, 0);
    EXPECT_EQ(Decode(huge).error, DecodeError::TooLarge);

    EXPECT_EQ(Decode(WithHeader(0, 0, {})).error, DecodeError::UnknownType);
    EXPECT_EQ(Decode(WithHeader(999, 0, {})).error, DecodeError::UnknownType);

    auto hb = Encode(Heartbeat{ 1, 2 });
    hb[2] = 1;  // flags low byte
    EXPECT_EQ(Decode(hb).error, DecodeError::BadFlags);
}

TEST(Protocol, RejectsInvalidFieldValues)
{
    // Invalid enum in Reject.
    {
        ByteWriter w;
        w.U16(0xFFFF);
        w.U16(1);
        w.U16(0);
        w.String("x");
        EXPECT_EQ(Decode(WithHeader(3, 0, w.Take())).error, DecodeError::InvalidField);
    }
    // Bool encoded as 2.
    {
        auto bytes = Encode(SampleMessages()[1]);  // Welcome ends with the resumed bool
        bytes.back() = 2;
        EXPECT_EQ(Decode(bytes).error, DecodeError::InvalidField);
    }
    // Player id 0 in PlayerLeft.
    EXPECT_FALSE(Decode(Encode(PlayerLeft{ 0, LeaveReason::Quit })).Ok());
    // Snapshot claiming more entries than the hard limit.
    {
        ByteWriter w;
        w.U16(kMaxPlayersLimit + 1);
        EXPECT_FALSE(Decode(WithHeader(4, 0, w.Take())).Ok());
    }
    // Bad magic in Hello.
    {
        auto bytes = Encode(SampleMessages()[0]);
        bytes[4] ^= 0xFF;
        EXPECT_FALSE(Decode(bytes).Ok());
    }
}

TEST(Protocol, RejectsNonFiniteOrHugeTransforms)
{
    PlayerTransform t;
    t.position = { std::numeric_limits<float>::quiet_NaN(), 0, 0 };
    EXPECT_FALSE(Decode(Encode(t)).Ok());
    t.position = { std::numeric_limits<float>::infinity(), 0, 0 };
    EXPECT_FALSE(Decode(Encode(t)).Ok());
    t.position = { 2.0e7f, 0, 0 };
    EXPECT_FALSE(Decode(Encode(t)).Ok());
    t.position = { 0, 0, 0 };
    t.rotation = { 0, 0, 5000.0f };
    EXPECT_FALSE(Decode(Encode(t)).Ok());
}

TEST(Protocol, HelloFromFutureMajorDecodesFrozenPrefixOnly)
{
    ByteWriter w;
    w.U32(kMagic);
    w.U16(kProtocolMajor + 1);
    w.U16(0);
    w.U8(0xAB);  // unknown future layout
    w.U8(0xCD);
    const auto r = Decode(WithHeader(1, 0, w.Take()));
    ASSERT_TRUE(r.Ok());
    EXPECT_EQ(std::get<Hello>(*r.message).protocolMajor, kProtocolMajor + 1);
}

TEST(Protocol, TextValidation)
{
    EXPECT_TRUE(IsValidText("hello", 32));
    EXPECT_TRUE(IsValidText("日本語", 32));
    EXPECT_FALSE(IsValidText("tab\there", 32));
    EXPECT_FALSE(IsValidText("del\x7f", 32));
    EXPECT_FALSE(IsValidText("\xC0\xAF", 32));          // overlong '/'
    EXPECT_FALSE(IsValidText("\xED\xA0\x80", 32));      // UTF-16 surrogate
    EXPECT_FALSE(IsValidText("\xF4\x90\x80\x80", 32));  // > U+10FFFF
    EXPECT_FALSE(IsValidText("\xC2\x85", 32));          // C1 control (NEL)
    EXPECT_FALSE(IsValidText("\xE6\x97", 32));          // truncated sequence
    EXPECT_FALSE(IsValidText(std::string(33, 'a'), 32));

    EXPECT_TRUE(IsValidPlayerName("Ada"));
    EXPECT_FALSE(IsValidPlayerName(""));
    EXPECT_FALSE(IsValidPlayerName(" Ada"));
    EXPECT_FALSE(IsValidPlayerName("Ada "));
}

TEST(Protocol, SequenceComparisonHandlesWrap)
{
    EXPECT_TRUE(SequenceNewer(2, 1));
    EXPECT_FALSE(SequenceNewer(1, 2));
    EXPECT_FALSE(SequenceNewer(5, 5));
    EXPECT_TRUE(SequenceNewer(0, 0xFFFFFFFFu));
    EXPECT_TRUE(SequenceNewer(10, 0xFFFFFFF0u));
}

TEST(GameRuntime, PackingMatchesSfse)
{
    EXPECT_EQ(game::MakeRuntimeVersion(1, 16, 244), 0x01100F40u);
    EXPECT_EQ(game::MakeRuntimeVersion(1, 15, 222), 0x010F0DE0u);
    EXPECT_EQ(game::FormatRuntimeVersion(game::kTargetRuntime), "1.16.244");
    EXPECT_EQ(game::ParseRuntimeVersion("1.16.244"), game::kTargetRuntime);
    EXPECT_FALSE(game::ParseRuntimeVersion("1.16"));
    EXPECT_FALSE(game::ParseRuntimeVersion("1.16.244.0"));
    EXPECT_FALSE(game::ParseRuntimeVersion("1.16.5000"));
    EXPECT_FALSE(game::ParseRuntimeVersion("a.b.c"));
}
