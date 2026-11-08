#include "ShipSync.h"

#include "Avatars.h"
#include "GameState.h"

#include "cosync/shared/Log.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <atomic>
#include <cmath>
#include <memory>

namespace cosync::plugin::ships
{
    namespace
    {
        using namespace std::chrono_literals;

        constexpr std::uint64_t kInSpaceId = 63482;  // 0xB58D50, ObjectReference.IsInSpace
        constexpr auto kLogEvery = 10s;
        constexpr auto kStaleAfter = 3s;
        constexpr auto kApplyEvery = 100ms;
        constexpr float kMovedBy = 1.0f;        // a remote ship that moved less than this is standing still
        constexpr float kSamePlace = 20000.0f;  // further apart than this, the ships are not in one place

        bool InSpace(RE::TESObjectREFR* ref)
        {
            using Fn = bool (*)(RE::TESObjectREFR*, bool);
            static REL::Relocation<Fn> fn{ REL::ID(kInSpaceId) };
            return fn(ref, false);
        }

        // Game.GetPlayerHomeSpaceShip, asked once: GetSpaceship only finds the ship while the player
        // is aboard (2026-11-07: none for a player standing next to the landed ship).
        const auto g_home = std::make_shared<std::atomic<std::uint32_t>>(0);
        bool g_asked = false;

        // The player's home ship, wherever it is.
        RE::TESObjectREFR* LocalShipRef()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->parentCell) return nullptr;
            auto* ship = player->GetSpaceship(true);
            if (!ship) {
                if (!g_asked) g_asked = avatars::RequestHomeShip(g_home);
                if (const auto id = g_home->load(); id > 1) {
                    auto* form = RE::TESForm::LookupByID(id);
                    ship = avatars::AsRef(form);
                }
            }
            return ship && ship->parentCell && ship->GetBaseObject() ? ship : nullptr;
        }

        float Distance(const Vec3& a, const Vec3& b)
        {
            return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z);
        }
    }

    std::optional<proto::ShipState> ReadLocalShip()
    {
        auto* ship = LocalShipRef();
        if (!ship) return std::nullopt;
        const auto at = ship->GetPosition();
        const auto angle = ship->GetAngle();
        return proto::ShipState{ StableCellId(ship->parentCell->GetFormID()), ship->GetBaseObject()->GetFormID(), { at.x, at.y, at.z },
                                 { angle.x, angle.y, angle.z } };
    }

    void Replication::OnRemote(PlayerId player, const std::optional<proto::ShipState>& ship)
    {
        if (!ship) return;
        auto& r = _remote[player];
        // Did it move? Only a ship that moves on its owner's copy is driven here, so two copies whose
        // ships both stand still never pull at each other.
        if (r.samples > 0 && Distance(ship->position, r.ship.position) > kMovedBy) r.moved = true;
        r.ship = *ship;
        r.at = Clock::now();
        ++r.samples;
    }

    // Every player runs the same save, so the remote player's home ship is this copy's home ship too:
    // when they move it, it moves here. Showing a second, separate ship is the stand-in problem in
    // docs/SHIP_SYNC.md and is not done.
    void Replication::Tick()
    {
        const auto now = Clock::now();
        std::erase_if(_remote, [&](const auto& entry) { return now - entry.second.at > kStaleAfter; });
        if (_remote.empty()) return;
        auto* local = LocalShipRef();
        if (local && now >= _nextApply) {
            _nextApply = now + kApplyEvery;
            for (auto& [player, r] : _remote) {
                if (!r.moved || r.ship.baseId != local->GetBaseObject()->GetFormID()) continue;
                r.moved = false;
                const auto at = local->GetPosition();
                const Vec3 here{ at.x, at.y, at.z };
                if (Distance(here, r.ship.position) > kSamePlace || Distance(here, r.ship.position) <= kMovedBy) continue;
                avatars::PlaceRef(local->GetFormID(), r.ship.position, r.ship.rotation);
                ++_applied;
            }
        }
        if (now < _nextLog) return;
        _nextLog = now + kLogEvery;
        const auto mine = ReadLocalShip();
        for (const auto& [player, r] : _remote) {
            const auto& s = r.ship;
            if (!mine) {
                log::Info("[ship] player {} has {:08X} in cell {:08X} at {:.1f} {:.1f} {:.1f}; no ship here ({} samples)", player, s.baseId, s.cellId, s.position.x,
                          s.position.y, s.position.z, r.samples);
                continue;
            }
            log::Info("[ship] player {} has {:08X} in cell {:08X} at {:.1f} {:.1f} {:.1f}; this ship {:08X} at {:.1f} {:.1f} {:.1f}, {:.1f} apart, in space {} ({} samples, {} moves applied)",
                      player, s.baseId, s.cellId, s.position.x, s.position.y, s.position.z, mine->baseId, mine->position.x, mine->position.y, mine->position.z,
                      Distance(s.position, mine->position), local && InSpace(local), r.samples, _applied);
        }
    }
}
