#include "MenuPause.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <array>
#include <atomic>
#include <cstring>

namespace cosync::plugin::menupause
{
    namespace
    {
        constexpr std::uint64_t kMainUpdateId = 99406;   // 0x1888410
        constexpr std::size_t kCounterJumpOffset = 0x64;  // ja: pause if UI+0x4B4 != 0
        constexpr std::size_t kMenuCheckOffset = 0x71;    // call 0x2544D60 (is MainMenu open)
        constexpr std::size_t kPauseCounterOffset = 0x4B4;
        constexpr std::array<std::uint8_t, 9> kExpectedCmp{ 0x44, 0x39, 0xA3, 0xB4, 0x04, 0x00, 0x00, 0x77, 0x3F };

        using IsMenuOpen_t = bool (*)(void* ui, const RE::BSFixedString* name);
        IsMenuOpen_t g_isMenuOpen = nullptr;
        std::atomic<bool> g_active{ false };

        // Stands in for the check at the call site, which now also decides the skipped counter test.
        bool MenuCheck(void* ui, const RE::BSFixedString* name)
        {
            if (g_isMenuOpen(ui, name)) return true;
            const auto counter = *reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(ui) + kPauseCounterOffset);
            if (counter == 0) return false;
            if (!g_active.load(std::memory_order_relaxed)) return true;
            static const RE::BSFixedString loading("LoadingMenu");
            return g_isMenuOpen(ui, &loading);
        }
    }

    bool Install(std::string& detail)
    {
        if (g_isMenuOpen) {
            detail = "menu pause patch already installed";
            return true;
        }
        const REL::Relocation<std::uintptr_t> update{ REL::ID(kMainUpdateId) };
        const auto cmp = update.address() + kCounterJumpOffset - 7;
        if (std::memcmp(reinterpret_cast<const void*>(cmp), kExpectedCmp.data(), kExpectedCmp.size()) != 0) {
            detail = "menu pause patch refused: unexpected code in Main::Update";
            return false;
        }
        const auto call = update.address() + kMenuCheckOffset;
        if (*reinterpret_cast<const std::uint8_t*>(call) != 0xE8) {
            detail = "menu pause patch refused: no call at the menu check";
            return false;
        }
        REL::Relocation<std::uintptr_t> callSite{ call };
        g_isMenuOpen = reinterpret_cast<IsMenuOpen_t>(callSite.write_call<5>(&MenuCheck));
        // Only then stop the counter test from jumping: the hook takes over its job.
        const std::array<std::uint8_t, 2> nops{ 0x90, 0x90 };
        REL::Relocation<std::uintptr_t> jump{ update.address() + kCounterJumpOffset };
        jump.write(std::span<const std::uint8_t>(nops));
        detail = "menu pause patch installed";
        return true;
    }

    void SetActive(bool active) noexcept { g_active.store(active, std::memory_order_relaxed); }
}
