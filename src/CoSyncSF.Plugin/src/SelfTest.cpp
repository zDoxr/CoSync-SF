#include "SelfTest.h"

#include "NpcSync.h"

#include "cosync/shared/Log.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <string>

namespace cosync::plugin::selftest
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;

        constexpr std::uint64_t kMainId = 883591;   // Main singleton (RE 12.8)
        constexpr std::size_t kPausedOffset = 0x448;
        constexpr std::size_t kPauseCounterOffset = 0x4B4;
        constexpr auto kOpenFor = 12s;
        constexpr auto kGap = 8s;
        // Console last: it stays open after a hide message. Not InventoryMenu: shown without the
        // game's own setup it reads a null pointer (0x15CB540), which the real key does not.
        constexpr std::array kMenus{ "PauseMenu", "DataMenu", "GalaxyStarMapMenu", "Console" };

        std::uint32_t g_delay = 0;
        std::optional<Clock::time_point> g_readyAt;
        std::size_t g_menu = 0;
        bool g_open = false;
        bool g_done = false;
        Clock::time_point g_nextStep{};
        Clock::time_point g_nextSample{};
        std::map<std::uint32_t, Vec3> g_last;
        float g_moved = 0.0f;
        std::uint32_t g_samples = 0, g_pausedSamples = 0, g_counterSamples = 0;

        bool ReadStack(const std::uintptr_t* at, std::uintptr_t& value) noexcept
        {
            __try {
                value = *at;
                return true;
            } __except (1) {
                return false;
            }
        }

        // Records faults to the log: some crashes leave no dump. Only observes, never handles.
        LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
        {
            static std::atomic<int> logged{ 0 };
            const auto code = info->ExceptionRecord->ExceptionCode;
            if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
                code != 0xC0000409 /* stack buffer overrun, fast fail */) {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            if (logged.fetch_add(1) >= 32) return EXCEPTION_CONTINUE_SEARCH;
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            const auto at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
            const auto rsp = reinterpret_cast<const std::uintptr_t*>(info->ContextRecord->Rsp);
            std::string stack;
            for (int i = 0; i < 64 && stack.size() < 160; ++i) {
                std::uintptr_t v = 0;
                if (!ReadStack(rsp + i, v)) break;
                if (v > base && v < base + 0x8000000) stack += std::format(" {:#x}", v - base);
            }
            log::Warn("[fault] code {:08X} at game+{:#x} (thread {}), access {:#x}; stack:{}", static_cast<std::uint32_t>(code),
                      at - base, GetCurrentThreadId(), info->ExceptionRecord->NumberParameters > 1 ? info->ExceptionRecord->ExceptionInformation[1] : 0, stack);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        void Show(const char* name, RE::UI_MESSAGE_TYPE type)
        {
            if (auto* queue = RE::UIMessageQueue::GetSingleton()) queue->AddMessage(RE::BSFixedString(name), type);
        }

        void Sample()
        {
            const auto e = npcsync::EnumerateHighActors();
            float moved = 0.0f;
            std::map<std::uint32_t, Vec3> now;
            for (const auto& a : e.actors) {
                now[a.formId] = a.position;
                if (const auto it = g_last.find(a.formId); it != g_last.end()) {
                    moved += std::hypot(a.position.x - it->second.x, a.position.y - it->second.y);
                }
            }
            g_last = std::move(now);
            g_moved += moved;
            ++g_samples;
            auto* main = *REL::Relocation<std::byte**>{ REL::ID(kMainId) };
            if (main && *reinterpret_cast<const std::uint8_t*>(main + kPausedOffset) != 0) ++g_pausedSamples;
            const auto* ui = reinterpret_cast<const std::byte*>(RE::UI::GetSingleton());
            if (ui && *reinterpret_cast<const std::uint32_t*>(ui + kPauseCounterOffset) != 0) ++g_counterSamples;
        }

        void Report(const char* what)
        {
            log::Info("[test] {}: {} samples, engine paused in {}, pause counter up in {}, NPCs moved {:.1f} in total",
                      what, g_samples, g_pausedSamples, g_counterSamples, g_moved);
            g_moved = 0.0f;
            g_samples = g_pausedSamples = g_counterSamples = 0;
        }
    }

    void Configure(std::uint32_t delaySeconds) noexcept
    {
        g_delay = delaySeconds;
        if (g_delay != 0) AddVectoredExceptionHandler(1, OnException);
    }

    void Tick(bool ready)
    {
        if (g_delay == 0 || g_done) return;
        const auto now = Clock::now();
        if (!ready) {
            g_readyAt.reset();
            return;
        }
        if (!g_readyAt) {
            g_readyAt = now;
            g_nextStep = now + std::chrono::seconds(g_delay);
            log::Info("[test] menu test starts in {} s", g_delay);
        }
        if (now >= g_nextSample) {
            g_nextSample = now + 1s;
            Sample();
        }
        if (now < g_nextStep) {
            // Idle: keeps the world's motion visible for menus opened by hand.
            if (g_samples >= 5 && !g_open) Report("waiting");
            return;
        }
        if (g_open) {
            const auto* name = kMenus[g_menu];
            Report(name);
            Show(name, RE::UI_MESSAGE_TYPE::kHide);
            g_open = false;
            ++g_menu;
            g_nextStep = now + kGap;
            if (g_menu >= kMenus.size()) {
                g_done = true;
                log::Info("[test] menu test finished");
            }
            return;
        }
        Report("no menu");
        const auto* name = kMenus[g_menu];
        log::Info("[test] opening {}", name);
        Show(name, RE::UI_MESSAGE_TYPE::kShow);
        g_open = true;
        g_nextStep = now + kOpenFor;
    }
}
