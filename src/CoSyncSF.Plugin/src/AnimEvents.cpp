#include "AnimEvents.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <cstddef>
#include <deque>
#include <format>
#include <mutex>

namespace cosync::plugin::animevents
{
    namespace
    {
        constexpr std::uint64_t kPlayerAnimSinkVtableId = 452451;  // PlayerCharacter vtable, COL offset 0x50
        constexpr std::size_t kProcessEventSlot = 1;
        constexpr std::size_t kTagOffset = 0x08;
        constexpr std::size_t kPayloadOffset = 0x10;
        constexpr std::size_t kHolderOffset = 0x60;  // IAnimationGraphManagerHolder in TESObjectREFR
        constexpr std::size_t kNotifySlot = 1;
        constexpr std::size_t kMaxQueued = 512;
        constexpr std::uint64_t kPlayerHolderVtableId = 452463;  // PlayerCharacter vtable, COL offset 0x60

        using ProcessEvent_t = std::uint32_t (*)(void* sink, const void* event, void* source);
        ProcessEvent_t g_original = nullptr;
        bool g_installed = false;

        std::mutex g_lock;
        std::deque<CapturedEvent> g_queue;
        std::uint64_t g_dropped = 0;

        std::string Read(const void* event, std::size_t offset)
        {
            const auto* s = reinterpret_cast<const RE::BSFixedString*>(static_cast<const std::byte*>(event) + offset);
            const char* text = s->c_str();
            return text ? std::string(text) : std::string();
        }

        // Runs on whichever thread the engine processes animation events on.
        std::uint32_t Hook(void* sink, const void* event, void* source)
        {
            if (event) {
                CapturedEvent e{ Read(event, kTagOffset), Read(event, kPayloadOffset) };
                std::lock_guard lock(g_lock);
                if (g_queue.size() < kMaxQueued) g_queue.push_back(std::move(e));
                else ++g_dropped;
            }
            return g_original(sink, event, source);
        }
    }

    namespace
    {
        using NotifyHook_t = bool (*)(void* holder, const RE::BSFixedString* name, void* extra);
        NotifyHook_t g_originalNotify = nullptr;
        bool g_inputInstalled = false;

        // Runs on whichever thread sends the event (game or animation threads).
        bool NotifyHook(void* holder, const RE::BSFixedString* name, void* extra)
        {
            const bool accepted = g_originalNotify(holder, name, extra);
            if (name) {
                const char* text = name->c_str();
                CapturedEvent e{ text ? std::string(text) : std::string(), std::string(), true, accepted };
                std::lock_guard lock(g_lock);
                if (g_queue.size() < kMaxQueued) g_queue.push_back(std::move(e));
                else ++g_dropped;
            }
            return accepted;
        }
    }

    bool InstallPlayerInputCapture(std::string& detail)
    {
        if (g_inputInstalled) {
            detail = "player graph-input capture already installed";
            return true;
        }
        REL::Relocation<std::uintptr_t> vtbl{ REL::ID(kPlayerHolderVtableId) };
        g_originalNotify = reinterpret_cast<NotifyHook_t>(vtbl.write_vfunc(kNotifySlot, &NotifyHook));
        g_inputInstalled = true;
        detail = std::format("player graph-input capture installed (vtable ID {} slot {})", kPlayerHolderVtableId,
                             kNotifySlot);
        return true;
    }

    bool InstallPlayerCapture(std::string& detail)
    {
        if (g_installed) {
            detail = "player animation-event capture already installed";
            return true;
        }
        REL::Relocation<std::uintptr_t> vtbl{ REL::ID(kPlayerAnimSinkVtableId) };
        const auto previous = vtbl.write_vfunc(kProcessEventSlot, &Hook);
        g_original = reinterpret_cast<ProcessEvent_t>(previous);
        g_installed = true;
        detail = std::format("player animation-event capture installed (vtable ID {} slot {})", kPlayerAnimSinkVtableId,
                             kProcessEventSlot);
        return true;
    }

    void Drain(std::vector<CapturedEvent>& out, std::uint64_t& dropped)
    {
        std::lock_guard lock(g_lock);
        while (!g_queue.empty()) {
            out.push_back(std::move(g_queue.front()));
            g_queue.pop_front();
        }
        dropped = g_dropped;
    }

    bool Notify(std::uint32_t refId, const char* eventName)
    {
        auto* form = RE::TESForm::LookupByID(refId);
        if (!form || !form->Is(RE::FormType::kACHR)) return false;
        auto* holder = reinterpret_cast<std::byte*>(form) + kHolderOffset;
        using Notify_t = bool (*)(void* holder, const RE::BSFixedString& name, void* extra);
        const auto vtbl = *reinterpret_cast<void* const* const*>(holder);
        const RE::BSFixedString name(eventName);
        return reinterpret_cast<Notify_t>(vtbl[kNotifySlot])(holder, name, nullptr);
    }
}
