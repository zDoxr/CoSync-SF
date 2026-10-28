// The only translation unit that touches Starfield engine objects.

#include "GameState.h"

#include <SFSE/SFSE.h>

#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESObjectCELL.h>

namespace cosync::plugin
{
    std::optional<LocalPlayerSample> ReadLocalPlayer()
    {
        // ID::PlayerCharacter::Singleton via the Address Library.
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return std::nullopt;
        }
        // No parent cell: not in the world yet (main menu) or between cells.
        const auto* cell = player->parentCell;
        if (!cell) {
            return std::nullopt;
        }

        // Plain member reads (TESObjectREFR::data); no engine calls.
        const auto pos = player->GetPosition();
        const auto ang = player->GetAngle();

        LocalPlayerSample sample;
        sample.cellFormId = cell->GetFormID();
        sample.position = { pos.x, pos.y, pos.z };
        sample.rotation = { ang.x, ang.y, ang.z };
        return sample;
    }
}
