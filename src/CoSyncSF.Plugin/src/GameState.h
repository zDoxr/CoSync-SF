#pragma once

#include "cosync/shared/Types.h"

#include <cstdint>
#include <optional>

namespace cosync::plugin
{
    struct LocalPlayerSample
    {
        // FormID of the player's parent cell. Load-order dependent: only
        // meaningful between players with identical plugin lists (see ROADMAP).
        std::uint32_t cellFormId = 0;
        Vec3 position;
        Vec3 rotation;  // radians, engine Euler angles
    };

    // Reads the local player's transform. MAIN THREAD ONLY.
    // Returns nullopt when there is no player or it is not in a loaded cell
    // (main menu, loading screens). Must not be called unless the Address
    // Library is installed (see AddressLibraryPresent).
    std::optional<LocalPlayerSample> ReadLocalPlayer();
}
