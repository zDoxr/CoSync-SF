#pragma once

#include "cosync/shared/Types.h"

#include <cstdint>

// Where each player is, and whether two players can see the same things (roadmap priority 5).
// Free of the game so the server and the tests can use it.
namespace cosync::presence
{
    // Location keys from the game layer (GameState.cpp): the parent cell's stable ID, with bit 31 set
    // for an exterior cell. Exterior cells are small, so two players outdoors can be cells apart and
    // still see each other. Distance decides there.
    inline constexpr std::uint32_t kExteriorBit = 0x80000000u;
    // Generous on purpose: sending a player something they cannot see costs bandwidth, while not
    // sending something they can see breaks sync.
    inline constexpr float kNearbyDistance = 8192.0f;

    struct Place
    {
        std::uint32_t locationId = 0;  // 0 = not known yet
        Vec3 position;
    };

    // False only when the two places are known to be apart. Two different exterior worldspaces can
    // share coordinates, so this can say "may share" for players on different planets. That costs
    // traffic, never correctness.
    [[nodiscard]] bool MayShare(const Place& a, const Place& b) noexcept;
}
