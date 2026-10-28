#include "cosync/shared/FormKey.h"
#include "cosync/shared/Interpolation.h"

#include <gtest/gtest.h>

#include <numbers>

using namespace cosync;

namespace
{
    TimedTransform At(std::uint64_t ms, std::uint32_t seq, float x, float yaw = 0.0f, std::uint32_t loc = 1)
    {
        TimedTransform t;
        t.receivedMs = ms;
        t.sequence = seq;
        t.locationId = loc;
        t.position = { x, 0.0f, 0.0f };
        t.rotation = { 0.0f, 0.0f, yaw };
        return t;
    }
}

TEST(Interpolation, EmptyBufferHasNoPose)
{
    TransformInterpolator interp;
    EXPECT_FALSE(interp.Sample(1000));
}

TEST(Interpolation, InterpolatesBehindNewestSample)
{
    TransformInterpolator interp({ .delayMs = 100 });
    interp.Push(At(1000, 1, 0.0f));
    interp.Push(At(1050, 2, 100.0f));
    interp.Push(At(1100, 3, 200.0f));

    // now 1175 -> render 1075: halfway between samples 2 and 3
    const auto pose = interp.Sample(1175);
    ASSERT_TRUE(pose);
    EXPECT_NEAR(pose->position.x, 150.0f, 0.01f);
    EXPECT_FALSE(pose->extrapolated);
    EXPECT_FALSE(pose->stale);
}

TEST(Interpolation, ExtrapolatesThenGoesStale)
{
    TransformInterpolator interp({ .delayMs = 100, .maxExtrapolateMs = 250 });
    interp.Push(At(1000, 1, 0.0f));
    interp.Push(At(1050, 2, 100.0f));

    auto pose = interp.Sample(1050 + 100 + 25);  // 25 ms past newest
    ASSERT_TRUE(pose);
    EXPECT_TRUE(pose->extrapolated);
    EXPECT_NEAR(pose->position.x, 150.0f, 0.01f);

    pose = interp.Sample(1050 + 100 + 300);  // beyond the extrapolation cap
    ASSERT_TRUE(pose);
    EXPECT_TRUE(pose->stale);
    EXPECT_NEAR(pose->position.x, 100.0f, 0.01f);  // holds the last real pose
}

TEST(Interpolation, AnglesTakeShortestArc)
{
    constexpr float pi = std::numbers::pi_v<float>;
    EXPECT_NEAR(TransformInterpolator::AngleDelta(pi - 0.1f, -pi + 0.1f), 0.2f, 1e-4f);
    EXPECT_NEAR(TransformInterpolator::AngleDelta(-pi + 0.1f, pi - 0.1f), -0.2f, 1e-4f);

    TransformInterpolator interp({ .delayMs = 0 });
    interp.Push(At(1000, 1, 0.0f, pi - 0.1f));
    interp.Push(At(1100, 2, 0.0f, -pi + 0.1f));
    const auto pose = interp.Sample(1050);
    ASSERT_TRUE(pose);
    // Halfway along the short arc is +/-pi, not 0.
    EXPECT_NEAR(std::abs(pose->rotation.z), pi, 1e-3f);
}

TEST(Interpolation, TeleportAndLocationChangeSnap)
{
    TransformInterpolator interp({ .delayMs = 0, .teleportDistance = 1000.0f });
    interp.Push(At(1000, 1, 0.0f));
    interp.Push(At(1100, 2, 5000.0f));  // jump
    auto pose = interp.Sample(1050);
    ASSERT_TRUE(pose);
    EXPECT_NEAR(pose->position.x, 0.0f, 0.01f);  // not 2500

    TransformInterpolator cells({ .delayMs = 0 });
    cells.Push(At(1000, 1, 0.0f, 0.0f, 1));
    cells.Push(At(1100, 2, 10.0f, 0.0f, 2));  // different cell
    pose = cells.Sample(1050);
    ASSERT_TRUE(pose);
    EXPECT_EQ(pose->locationId, 1u);
    EXPECT_NEAR(pose->position.x, 0.0f, 0.01f);
    pose = cells.Sample(1100);
    ASSERT_TRUE(pose);
    EXPECT_EQ(pose->locationId, 2u);
}

TEST(Interpolation, RejectsOldSequencesAndBoundsMemory)
{
    TransformInterpolator interp({ .capacity = 4 });
    EXPECT_TRUE(interp.Push(At(1000, 5, 0.0f)));
    EXPECT_FALSE(interp.Push(At(1010, 5, 1.0f)));  // duplicate
    EXPECT_FALSE(interp.Push(At(1020, 4, 1.0f)));  // older
    for (std::uint32_t i = 6; i < 100; ++i) {
        interp.Push(At(1000 + i * 10, i, static_cast<float>(i)));
    }
    EXPECT_EQ(interp.Size(), 4u);
}

TEST(FormKey, DecomposesAllFileKinds)
{
    auto p = forms::Decompose(0x0001A2B3);
    EXPECT_EQ(p.kind, forms::FileKind::Full);
    EXPECT_EQ(p.fileIndex, 0u);
    EXPECT_EQ(p.localId, 0x01A2B3u);

    p = forms::Decompose(0x05ABCDEF);
    EXPECT_EQ(p.fileIndex, 5u);
    EXPECT_EQ(p.localId, 0xABCDEFu);

    p = forms::Decompose(0xFD12ABCD);
    EXPECT_EQ(p.kind, forms::FileKind::Medium);
    EXPECT_EQ(p.fileIndex, 0x12u);
    EXPECT_EQ(p.localId, 0xABCDu);

    p = forms::Decompose(0xFE123456);
    EXPECT_EQ(p.kind, forms::FileKind::Small);
    EXPECT_EQ(p.fileIndex, 0x123u);
    EXPECT_EQ(p.localId, 0x456u);

    p = forms::Decompose(0xFF000ABC);
    EXPECT_EQ(p.kind, forms::FileKind::Runtime);
}

TEST(FormKey, StableIdIgnoresCaseAndLoadOrder)
{
    // Same plugin + local id => same id regardless of case (and of load order,
    // which is not an input at all).
    EXPECT_EQ(forms::StableId("Starfield.esm", 0x1234), forms::StableId("STARFIELD.ESM", 0x1234));
    EXPECT_NE(forms::StableId("Starfield.esm", 0x1234), forms::StableId("Starfield.esm", 0x1235));
    EXPECT_NE(forms::StableId("Starfield.esm", 0x1234), forms::StableId("Constellation.esm", 0x1234));
    EXPECT_NE(forms::StableId("", 0), 0u);
    EXPECT_EQ(forms::Describe("Starfield.esm", 0x1A2B), "Starfield.esm:00001A2B");
}
