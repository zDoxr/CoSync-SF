#pragma once

#include "cosync/shared/Protocol.h"
#include "cosync/shared/Types.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <vector>

// Engine-independent decisions of NPC replication (docs/NPC_SYNC_ARCHITECTURE.md), kept here so
// they can be tested without the game.
namespace cosync::npc
{
    // A snapshot cut into messages of at most proto::kMaxNpcStates, in order.
    inline std::vector<std::vector<proto::NpcState>> SplitForWire(const std::vector<proto::NpcState>& npcs)
    {
        std::vector<std::vector<proto::NpcState>> parts;
        for (std::size_t i = 0; i < npcs.size(); i += proto::kMaxNpcStates) {
            const auto end = std::min(i + proto::kMaxNpcStates, npcs.size());
            parts.emplace_back(npcs.begin() + static_cast<std::ptrdiff_t>(i), npcs.begin() + static_cast<std::ptrdiff_t>(end));
        }
        return parts;
    }

    // What a replica does with one CrowdManager entry that follows the authority (RE 12.10).
    enum class CrowdStep : std::uint8_t
    {
        Run,       // the engine's state machine carries on
        Hold,      // skip the update: this is where it would take a random decision
        Retarget,  // the authority went elsewhere: assign that target, then run
    };

    // state: CrowdActorInfo +0x78. States 13 to 23 and 25 (despawning, reacting to a player, door
    // animations, dead) finish on their own first. Moving, arriving, waiting and choosing can all
    // be redirected: state 0 reissues the path, and it also stands a seated actor up.
    [[nodiscard]] inline CrowdStep DecideCrowdStep(std::uint32_t state, bool sameTarget) noexcept
    {
        constexpr std::uint32_t kRedirectable = 0x1FFFu | (1u << 24);  // 0..12, 24
        constexpr std::uint32_t kDeciding = (1u << 3) | (1u << 4) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10);
        if (state >= 32) return CrowdStep::Run;
        const std::uint32_t bit = 1u << state;
        if (sameTarget) return (bit & kDeciding) != 0 ? CrowdStep::Hold : CrowdStep::Run;
        return (bit & kRedirectable) != 0 ? CrowdStep::Retarget : CrowdStep::Run;
    }

    struct CrowdTarget
    {
        std::uint32_t handle = 0;
        Vec3 position;
    };

    // The local crowd target at the authority's position, or 0. Both copies place the same data,
    // so a real match is a near-exact one.
    [[nodiscard]] inline std::uint32_t FindCrowdTarget(const std::vector<CrowdTarget>& targets, const Vec3& at) noexcept
    {
        constexpr float kMatch = 0.25f;
        std::uint32_t best = 0;
        float bestSq = kMatch * kMatch;
        for (const auto& t : targets) {
            const float dx = t.position.x - at.x, dy = t.position.y - at.y, dz = t.position.z - at.z;
            const float sq = dx * dx + dy * dy + dz * dz;
            if (sq <= bestSq) {
                bestSq = sq;
                best = t.handle;
            }
        }
        return best;
    }

    // Outfits cut into messages of at most proto::kMaxNpcOutfits, in order.
    inline std::vector<std::vector<proto::NpcOutfit>> SplitOutfits(std::vector<proto::NpcOutfit> outfits)
    {
        std::vector<std::vector<proto::NpcOutfit>> parts;
        for (std::size_t i = 0; i < outfits.size(); i += proto::kMaxNpcOutfits) {
            const auto end = std::min(i + proto::kMaxNpcOutfits, outfits.size());
            parts.emplace_back(std::make_move_iterator(outfits.begin() + static_cast<std::ptrdiff_t>(i)),
                               std::make_move_iterator(outfits.begin() + static_cast<std::ptrdiff_t>(end)));
        }
        return parts;
    }

    // What to change so that `have` becomes `want`. Both sorted, without duplicates.
    struct OutfitChanges
    {
        std::vector<std::uint32_t> equip;
        std::vector<std::uint32_t> unequip;
        [[nodiscard]] bool Empty() const noexcept { return equip.empty() && unequip.empty(); }
    };
    inline OutfitChanges DiffOutfit(const std::vector<std::uint32_t>& want, const std::vector<std::uint32_t>& have)
    {
        OutfitChanges c;
        std::ranges::set_difference(want, have, std::back_inserter(c.equip));
        std::ranges::set_difference(have, want, std::back_inserter(c.unequip));
        return c;
    }

    // Keeps the `limit` NPCs nearest to `centre`, nearest first.
    inline void KeepNearest(std::vector<proto::NpcState>& npcs, const Vec3& centre, std::size_t limit)
    {
        const auto dist2 = [&](const proto::NpcState& n) {
            const float dx = n.position.x - centre.x, dy = n.position.y - centre.y, dz = n.position.z - centre.z;
            return dx * dx + dy * dy + dz * dz;
        };
        std::ranges::stable_sort(npcs, {}, dist2);
        if (npcs.size() > limit) npcs.resize(limit);
    }

    // Engine move input for a character that should follow an authoritative position:
    // desired velocity = target velocity + gain * position error, expressed the way
    // IMovementPlayerControls expects it (docs/ANIMATION_REVERSE_ENGINEERING.md A.10).
    struct Steering
    {
        float angle = 0.0f;      // ControlsData +0x08: direction relative to facing, atan2(right, forward)
        float magnitude = 0.0f;  // ControlsData +0x30: 0 stand .. 2 run
    };

    inline Steering Steer(const Vec3& error, float targetVelX, float targetVelY, float yaw)
    {
        constexpr float kGain = 2.0f;      // 1/s: closes a 1-unit error at 2 u/s extra
        constexpr float kRunSpeed = 5.5f;  // u/s at magnitude 2 (measured on the proxy, 2026-11-03)
        constexpr float kMaxMagnitude = 2.0f;
        const float targetSpeed = std::hypot(targetVelX, targetVelY);
        const float planarError = std::hypot(error.x, error.y);
        if (targetSpeed < 0.2f && planarError < 0.25f) return {};  // arrived: stand exactly still
        const float vx = targetVelX + kGain * error.x, vy = targetVelY + kGain * error.y;
        const float speed = std::hypot(vx, vy);
        if (speed <= 0.05f) return {};
        // Engine convention: forward = (sin yaw, cos yaw), right = (cos yaw, -sin yaw).
        const float forward = vx * std::sin(yaw) + vy * std::cos(yaw);
        const float right = vx * std::cos(yaw) - vy * std::sin(yaw);
        return { std::atan2(right, forward), std::min(speed / kRunSpeed * 2.0f, kMaxMagnitude) };
    }

    // One client's view of one NPC's health, in percent. Tells what the client's own player did to
    // its local copy apart from what the authority reported, so each drop is forwarded exactly once
    // and the authority's value always wins.
    class HealthTracker
    {
    public:
        // The health on this machine, before any authoritative value is applied. Returns the drop
        // since the last known value, to forward to the authority (0 = nothing new).
        float ObserveLocal(float localPercent)
        {
            if (!_known) return 0.0f;  // first sight: nothing to compare with
            const float drop = *_known - localPercent;
            _known = localPercent;
            return drop > kEpsilon ? drop : 0.0f;
        }

        // The authority's value. Returns the percent change to apply here, if any. Never lethal:
        // only the authority's death event kills a client's copy.
        std::optional<float> OnAuthoritative(float authorityPercent, float localPercent)
        {
            const float target = std::max(authorityPercent, kMinPercent);
            _known = target;
            const float change = target - localPercent;
            if (std::fabs(change) < kApplyThreshold) {
                _known = localPercent;
                return std::nullopt;
            }
            return change;
        }

    private:
        static constexpr float kEpsilon = 0.05f;
        static constexpr float kApplyThreshold = 1.0f;  // the wire carries whole percents
        static constexpr float kMinPercent = 1.0f;
        std::optional<float> _known;
    };
}
