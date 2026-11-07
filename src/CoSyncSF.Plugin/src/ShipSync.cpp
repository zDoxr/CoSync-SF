#include "ShipSync.h"

#include "GameState.h"

#include "cosync/shared/Log.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <cmath>

namespace cosync::plugin::ships
{
    namespace
    {
        using namespace std::chrono_literals;

        constexpr std::uint64_t kInSpaceId = 63482;  // 0xB58D50, ObjectReference.IsInSpace
        constexpr auto kLogEvery = 10s;
        constexpr auto kStaleAfter = 3s;

        bool InSpace(RE::TESObjectREFR* ref)
        {
            using Fn = bool (*)(RE::TESObjectREFR*, bool);
            static REL::Relocation<Fn> fn{ REL::ID(kInSpaceId) };
            return fn(ref, false);
        }
    }

    std::optional<proto::ShipState> ReadLocalShip()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell || !InSpace(player)) return std::nullopt;
        // With true this also finds the home ship while the player walks around inside it.
        auto* ship = player->GetSpaceship(true);
        if (!ship || !ship->parentCell || !InSpace(ship)) return std::nullopt;
        const auto base = ship->GetBaseObject();
        if (!base) return std::nullopt;
        const auto at = ship->GetPosition();
        const auto angle = ship->GetAngle();
        return proto::ShipState{ StableCellId(ship->parentCell->GetFormID()), base->GetFormID(), { at.x, at.y, at.z }, { angle.x, angle.y, angle.z } };
    }

    void Replication::OnRemote(PlayerId player, const std::optional<proto::ShipState>& ship)
    {
        if (!ship) return;
        auto& r = _remote[player];
        r.ship = *ship;
        r.at = Clock::now();
        ++r.samples;
    }

    void Replication::Tick()
    {
        const auto now = Clock::now();
        std::erase_if(_remote, [&](const auto& entry) { return now - entry.second.at > kStaleAfter; });
        if (_remote.empty() || now < _nextLog) return;
        _nextLog = now + kLogEvery;
        const auto local = ReadLocalShip();
        for (const auto& [player, r] : _remote) {
            const auto& s = r.ship;
            if (!local) {
                log::Info("[ship] player {} flies {:08X} in cell {:08X} at {:.1f} {:.1f} {:.1f}; this player is not in space ({} samples)", player, s.baseId,
                          s.cellId, s.position.x, s.position.y, s.position.z, r.samples);
                continue;
            }
            const float dx = s.position.x - local->position.x, dy = s.position.y - local->position.y, dz = s.position.z - local->position.z;
            log::Info("[ship] player {} flies {:08X} in cell {:08X} at {:.1f} {:.1f} {:.1f}; this ship {:08X} in cell {:08X}, {} cell, {:.1f} apart ({} samples)", player,
                      s.baseId, s.cellId, s.position.x, s.position.y, s.position.z, local->baseId, local->cellId, s.cellId == local->cellId ? "same" : "other",
                      std::sqrt(dx * dx + dy * dy + dz * dz), r.samples);
        }
    }
}
