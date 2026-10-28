#include "MultiInstance.h"

#include <Windows.h>

#include <cstring>
#include <format>

namespace cosync::plugin
{
    namespace
    {
        using FindWindowA_t = HWND(WINAPI*)(LPCSTR, LPCSTR);
        FindWindowA_t g_originalFindWindowA = nullptr;

        bool IsStarfieldClass(LPCSTR className)
        {
            // Class names can also be ATOMs (MAKEINTATOM); those are passed through.
            if (!className || IS_INTRESOURCE(className)) {
                return false;
            }
            return _stricmp(className, "Starfield") == 0;
        }

        HWND WINAPI FindWindowA_Hook(LPCSTR className, LPCSTR windowName)
        {
            if (IsStarfieldClass(className) && windowName == nullptr) {
                return nullptr;  // pretend no other copy is running
            }
            return g_originalFindWindowA(className, windowName);
        }

        // Finds the IAT slot of `dll!function` in `module` by walking its import
        // descriptors (by-name imports only). nullptr if not imported.
        void** FindImportSlot(HMODULE module, const char* dll, const char* function)
        {
            auto* base = reinterpret_cast<std::uint8_t*>(module);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
            const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (dir.VirtualAddress == 0) return nullptr;

            for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name;
                 ++desc) {
                if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0) continue;
                if (desc->OriginalFirstThunk == 0) return nullptr;  // no name table to match against
                auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
                auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
                for (; names->u1.AddressOfData; ++names, ++slots) {
                    if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                    const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                    if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) == 0) {
                        return reinterpret_cast<void**>(&slots->u1.Function);
                    }
                }
            }
            return nullptr;
        }
    }

    bool InstallMultiInstancePatchIfRequested(std::string& detail)
    {
        char flag[8]{};
        if (GetEnvironmentVariableA("COSYNC_MULTI_INSTANCE", flag, sizeof flag) == 0 || std::strcmp(flag, "1") != 0) {
            detail = "multi-instance mode off";
            return false;
        }
        if (g_originalFindWindowA) {
            detail = "multi-instance patch already installed";
            return true;
        }

        const HMODULE game = GetModuleHandleW(nullptr);  // Starfield.exe
        void** slot = FindImportSlot(game, "USER32.dll", "FindWindowA");
        if (!slot) {
            detail = "multi-instance requested, but Starfield.exe does not import USER32!FindWindowA; not patched";
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            detail = std::format("multi-instance requested, but VirtualProtect failed ({})", GetLastError());
            return false;
        }
        g_originalFindWindowA = reinterpret_cast<FindWindowA_t>(*slot);
        *slot = reinterpret_cast<void*>(&FindWindowA_Hook);
        VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

        detail = "multi-instance mode: Starfield.exe's FindWindowA(\"Starfield\") single-instance check bypassed";
        return true;
    }
}
