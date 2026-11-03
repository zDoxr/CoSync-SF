// The only translation unit that touches Starfield engine objects.

#include "GameState.h"

#include "cosync/shared/FormKey.h"

#include <SFSE/SFSE.h>

#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESWorldSpace.h>
#include <RE/T/TESFile.h>
#include <RE/T/TESObjectCELL.h>

#include <atomic>
#include <cstring>

namespace cosync::plugin
{
    namespace
    {
        std::atomic<RE::TESDataHandler*> g_dataHandler{ nullptr };

        // Maps a FormID to its defining plugin via the compiled file lists.
        // Plain bounds-checked reads; nullptr if the index is out of range.
        const RE::TESFile* FileForForm(const RE::TESDataHandler& handler, const forms::FormIdParts& parts)
        {
            const auto& collection = handler.compiledFileCollection;
            const RE::BSTArray<RE::TESFile*>* list = nullptr;
            switch (parts.kind) {
            case forms::FileKind::Full: list = &collection.files; break;
            case forms::FileKind::Medium: list = &collection.mediumFiles; break;
            case forms::FileKind::Small: list = &collection.smallFiles; break;
            case forms::FileKind::Runtime: return nullptr;
            }
            if (parts.fileIndex >= list->size()) {
                return nullptr;
            }
            return (*list)[parts.fileIndex];
        }

        void ResolveLocation(std::uint32_t formId, LocalPlayerSample& out)
        {
            out.cellFormId = formId;
            const auto parts = forms::Decompose(formId);
            const auto* handler = g_dataHandler.load();
            const RE::TESFile* file = handler ? FileForForm(*handler, parts) : nullptr;
            if (!file) {
                // Runtime-created or unresolvable: fall back to the raw id (only
                // meaningful between identical load orders).
                out.locationId = forms::StableId("<runtime>", formId);
                out.locationName = forms::Describe("<runtime>", formId);
                return;
            }
            const std::string_view name(file->fileName, strnlen(file->fileName, sizeof(file->fileName)));
            out.locationId = forms::StableId(name, parts.localId);
            out.locationName = forms::Describe(name, parts.localId);
        }
    }

    void SetDataHandler(void* dataHandler) noexcept
    {
        g_dataHandler.store(static_cast<RE::TESDataHandler*>(dataHandler));
    }

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

        // Location key: the cell (a data-file form, identical on every machine). Starfield's
        // worldspaces can be runtime-created per process (FF010D3F vs FF010C84 for the same place,
        // 2026-11-03), so they cannot be the key. Bit 31 marks an exterior cell (the engine reports a
        // parent worldspace: GetParentWorldSpace = 0xB4EB40, ID 63417, RE section 2; null in
        // interiors) so the game layer can keep players visible across exterior cell borders.
        LocalPlayerSample sample;
        ResolveLocation(cell->GetFormID(), sample);
        const bool exterior = player->GetParentWorldSpace() != nullptr;
        sample.locationId = exterior ? (sample.locationId | 0x80000000u) : (sample.locationId & 0x7FFFFFFFu);
        sample.position = { pos.x, pos.y, pos.z };
        sample.rotation = { ang.x, ang.y, ang.z };
        return sample;
    }
}
