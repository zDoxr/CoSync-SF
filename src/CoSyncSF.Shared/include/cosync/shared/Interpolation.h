#pragma once

#include "cosync/shared/Types.h"

#include <cstdint>
#include <deque>
#include <optional>

namespace cosync
{
    struct InterpolationConfig
    {
        std::uint32_t delayMs = 100;           // render this far behind the newest sample
        std::uint32_t maxExtrapolateMs = 250;  // then hold the last pose
        float teleportDistance = 2000.0f;      // engine units; larger jumps snap instead of sliding
        std::size_t capacity = 32;             // samples kept per player
    };

    struct TimedTransform
    {
        std::uint64_t receivedMs = 0;  // local monotonic receive time
        std::uint32_t sequence = 0;
        std::uint32_t locationId = 0;
        Vec3 position;
        Vec3 rotation;  // radians
    };

    struct InterpolatedPose
    {
        std::uint32_t locationId = 0;
        Vec3 position;
        Vec3 rotation;
        bool extrapolated = false;  // past the newest sample (packet loss / late)
        bool stale = false;         // beyond maxExtrapolateMs: holding the last pose
    };

    // Per-player jitter buffer: renders `delayMs` in the past, interpolating
    // between the two samples around that time. Angles take the shortest arc.
    // A location change or a jump beyond `teleportDistance` is not blended.
    class TransformInterpolator
    {
    public:
        explicit TransformInterpolator(InterpolationConfig config = {}) : _config(config) {}

        // Out-of-order or duplicate sequences are ignored. Returns true if stored.
        bool Push(const TimedTransform& sample);
        [[nodiscard]] std::optional<InterpolatedPose> Sample(std::uint64_t nowMs) const;
        void Clear() { _samples.clear(); }
        [[nodiscard]] std::size_t Size() const noexcept { return _samples.size(); }

        // Smallest signed difference b - a, wrapped to [-pi, pi].
        [[nodiscard]] static float AngleDelta(float a, float b) noexcept;

    private:
        InterpolationConfig _config;
        std::deque<TimedTransform> _samples;  // ascending receivedMs
    };
}
