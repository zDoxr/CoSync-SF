#include "cosync/shared/Interpolation.h"

#include "cosync/shared/Protocol.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cosync
{
    namespace
    {
        float Lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }

        Vec3 Lerp(const Vec3& a, const Vec3& b, float t) noexcept
        {
            return { Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t) };
        }

        Vec3 LerpAngles(const Vec3& a, const Vec3& b, float t) noexcept
        {
            return { a.x + TransformInterpolator::AngleDelta(a.x, b.x) * t,
                     a.y + TransformInterpolator::AngleDelta(a.y, b.y) * t,
                     a.z + TransformInterpolator::AngleDelta(a.z, b.z) * t };
        }

        float Distance(const Vec3& a, const Vec3& b) noexcept
        {
            const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        bool IsDiscontinuity(const TimedTransform& a, const TimedTransform& b, float teleportDistance) noexcept
        {
            return a.locationId != b.locationId || Distance(a.position, b.position) > teleportDistance;
        }

        InterpolatedPose PoseOf(const TimedTransform& s) noexcept { return { s.locationId, s.position, s.rotation }; }
    }

    float TransformInterpolator::AngleDelta(float a, float b) noexcept
    {
        constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
        float d = std::fmod(b - a, kTwoPi);
        if (d > std::numbers::pi_v<float>) d -= kTwoPi;
        if (d < -std::numbers::pi_v<float>) d += kTwoPi;
        return d;
    }

    bool TransformInterpolator::Push(const TimedTransform& sample)
    {
        if (!_samples.empty() && !proto::SequenceNewer(sample.sequence, _samples.back().sequence)) {
            return false;
        }
        TimedTransform s = sample;
        if (!_samples.empty() && s.receivedMs < _samples.back().receivedMs) {
            s.receivedMs = _samples.back().receivedMs;  // keep timestamps monotonic
        }
        _samples.push_back(s);
        while (_samples.size() > std::max<std::size_t>(_config.capacity, 2)) {
            _samples.pop_front();
        }
        return true;
    }

    std::optional<InterpolatedPose> TransformInterpolator::Sample(std::uint64_t nowMs) const
    {
        if (_samples.empty()) {
            return std::nullopt;
        }
        const std::uint64_t renderMs = nowMs > _config.delayMs ? nowMs - _config.delayMs : 0;
        const auto& newest = _samples.back();

        if (renderMs <= _samples.front().receivedMs) {
            return PoseOf(_samples.front());
        }

        if (renderMs >= newest.receivedMs) {
            // Past the newest sample: extrapolate from the last two (if continuous).
            InterpolatedPose pose = PoseOf(newest);
            const std::uint64_t ahead = renderMs - newest.receivedMs;
            if (ahead > _config.maxExtrapolateMs) {
                pose.stale = true;
                return pose;
            }
            if (_samples.size() >= 2) {
                const auto& prev = _samples[_samples.size() - 2];
                const auto span = newest.receivedMs - prev.receivedMs;
                if (span > 0 && !IsDiscontinuity(prev, newest, _config.teleportDistance)) {
                    const float t = static_cast<float>(ahead) / static_cast<float>(span);
                    pose.position = {
                        newest.position.x + (newest.position.x - prev.position.x) * t,
                        newest.position.y + (newest.position.y - prev.position.y) * t,
                        newest.position.z + (newest.position.z - prev.position.z) * t,
                    };
                    pose.rotation = LerpAngles(prev.rotation, newest.rotation, 1.0f + t);
                    pose.extrapolated = true;
                }
            }
            return pose;
        }

        // Find the pair bracketing renderMs.
        for (std::size_t i = 1; i < _samples.size(); ++i) {
            const auto& b = _samples[i];
            if (b.receivedMs < renderMs) continue;
            const auto& a = _samples[i - 1];
            if (IsDiscontinuity(a, b, _config.teleportDistance)) {
                return PoseOf(renderMs < b.receivedMs ? a : b);  // snap, never slide across a teleport
            }
            const auto span = b.receivedMs - a.receivedMs;
            const float t = span == 0 ? 1.0f : static_cast<float>(renderMs - a.receivedMs) / static_cast<float>(span);
            InterpolatedPose pose;
            pose.locationId = b.locationId;
            pose.position = Lerp(a.position, b.position, t);
            pose.rotation = LerpAngles(a.rotation, b.rotation, t);
            return pose;
        }
        return PoseOf(newest);
    }
}
