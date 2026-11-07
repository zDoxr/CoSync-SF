#pragma once

#include "cosync/shared/Protocol.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>

// Ships in space (issue #10). Every player runs the same save and so has the same home ship, so
// another player's ship can only be shown as a stand-in of the same design, placed where theirs is.
// Placing that stand-in needs the local player to be flying (RE 12.11), so for now this sends the
// local ship and reports where the others are against it. Main thread only.
namespace cosync::plugin::ships
{
    // The local player's home ship, landed or in space.
    [[nodiscard]] std::optional<proto::ShipState> ReadLocalShip();

    class Replication
    {
    public:
        void OnRemote(PlayerId player, const std::optional<proto::ShipState>& ship);
        void Forget(PlayerId player) { _remote.erase(player); }
        void Tick();

    private:
        using Clock = std::chrono::steady_clock;
        struct Remote
        {
            proto::ShipState ship;
            Clock::time_point at{};
            std::uint64_t samples = 0;
            bool moved = false;  // moved on its owner's copy since it was last applied here
        };
        std::map<PlayerId, Remote> _remote;
        Clock::time_point _nextLog{};
        Clock::time_point _nextApply{};
        std::uint64_t _applied = 0;
    };
}
