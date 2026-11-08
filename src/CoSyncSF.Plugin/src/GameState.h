#pragma once

#include "cosync/shared/Types.h"

#include <cstdint>
#include <optional>
#include <string>

namespace cosync::plugin
{
    struct LocalPlayerSample
    {
        // Load-order-independent id of the player's parent cell
        // (forms::StableId of plugin file + local FormID); 0 if unresolved.
        std::uint32_t locationId = 0;
        std::string locationName;  // "Starfield.esm:0001A2B3" for logs
        std::uint32_t cellFormId = 0;  // raw, load-order dependent; logs only
        Vec3 position;
        Vec3 rotation;  // radians, engine Euler angles
    };

    // The TESDataHandler pointer SFSE passes with kPostDataLoad. Must be set
    // before ReadLocalPlayer can resolve stable location ids.
    void SetDataHandler(void* dataHandler) noexcept;

    // Reads the local player's transform. MAIN THREAD ONLY.
    // Returns nullopt when there is no player or it is not in a loaded cell
    // (main menu, loading screens). Must not be called unless the Address
    // Library is installed (PlayerCharacter::GetSingleton uses it).
    std::optional<LocalPlayerSample> ReadLocalPlayer();

    // The load-order-independent key ReadLocalPlayer uses for a cell, for any cell. MAIN THREAD ONLY.
    [[nodiscard]] std::uint32_t StableCellId(std::uint32_t cellFormId);
}
