#include "AnimEvents.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <format>
#include <map>
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

    namespace
    {
        constexpr std::uint64_t kActorHolderVtableId = 451626;  // Actor vtable, COL offset 0x60
        constexpr std::size_t kFormIdOffset = 0x28;
        constexpr std::size_t kMaxNpcQueued = 4096;

        NotifyHook_t g_originalActorNotify = nullptr;
        std::atomic<bool> g_npcCapture{ false };
        std::atomic<std::uint64_t> g_blocked{ 0 };
        std::mutex g_npcLock;
        std::deque<NpcGraphEvent> g_npcQueue;
        std::uint64_t g_npcDropped = 0;
        // Indexed by the low FormID bits: runs on every graph event of every NPC, so no search and
        // no lock. A collision only means one more NPC is treated as locked.
        std::array<std::atomic<std::uint8_t>, 65536> g_locked{};
        thread_local bool t_replaying = false;
        std::mutex g_blockedLock;
        std::map<std::string, std::uint32_t> g_blockedNames;

        // The engine sends these to set up or re-anchor a graph on its own machine (runtime list,
        // 2026-11-04). Each copy needs its own, and replaying one would reset a live graph.
        bool IsHousekeeping(const char* name) noexcept
        {
            return std::strcmp(name, "InitializeMTGraph") == 0 || std::strcmp(name, "InstantInitializeMTGraph") == 0 ||
                   std::strcmp(name, "PositionChange") == 0;
        }

        bool ActorNotifyHook(void* holder, const RE::BSFixedString* name, void* extra)
        {
            const auto formId = *reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(holder) -
                                                                        kHolderOffset + kFormIdOffset);
            const char* text = name ? name->c_str() : nullptr;
            const bool housekeeping = text && IsHousekeeping(text);
            if (!t_replaying && !housekeeping && g_locked[formId & 0xFFFF].load(std::memory_order_relaxed)) {
                g_blocked.fetch_add(1, std::memory_order_relaxed);
                if (text) {
                    std::lock_guard lock(g_blockedLock);
                    if (g_blockedNames.size() < 256 || g_blockedNames.contains(text)) ++g_blockedNames[text];
                }
                return false;
            }
            const bool accepted = g_originalActorNotify(holder, name, extra);
            if (accepted && text && !housekeeping && g_npcCapture.load(std::memory_order_relaxed)) {
                {
                    std::lock_guard lock(g_npcLock);
                    if (g_npcQueue.size() < kMaxNpcQueued) g_npcQueue.push_back({ formId, text });
                    else ++g_npcDropped;
                }
            }
            return accepted;
        }
    }

    bool InstallNpcCapture(std::string& detail)
    {
        if (g_originalActorNotify) {
            detail = "NPC graph hook already installed";
            return true;
        }
        REL::Relocation<std::uintptr_t> vtbl{ REL::ID(kActorHolderVtableId) };
        // Original first, then patch: the hook must never run with a null original.
        g_originalActorNotify = reinterpret_cast<NotifyHook_t>(reinterpret_cast<void* const*>(vtbl.address())[kNotifySlot]);
        vtbl.write_vfunc(kNotifySlot, &ActorNotifyHook);
        detail = std::format("NPC graph hook installed (vtable ID {} slot {})", kActorHolderVtableId, kNotifySlot);
        return true;
    }

    void SetNpcCapture(bool enable) noexcept { g_npcCapture.store(enable, std::memory_order_relaxed); }

    void DrainNpcEvents(std::vector<NpcGraphEvent>& out, std::uint64_t& dropped)
    {
        std::lock_guard lock(g_npcLock);
        while (!g_npcQueue.empty()) {
            out.push_back(std::move(g_npcQueue.front()));
            g_npcQueue.pop_front();
        }
        dropped = g_npcDropped;
    }

    void SetGraphLocked(std::uint32_t formId, bool locked) noexcept
    {
        g_locked[formId & 0xFFFF].store(locked ? 1 : 0, std::memory_order_relaxed);
    }

    bool NotifyLocked(std::uint32_t refId, const char* eventName)
    {
        t_replaying = true;
        const bool ok = Notify(refId, eventName);
        t_replaying = false;
        return ok;
    }

    std::uint64_t BlockedLocalEvents() noexcept { return g_blocked.load(std::memory_order_relaxed); }

    std::map<std::string, std::uint32_t> BlockedEventNames()
    {
        std::lock_guard lock(g_blockedLock);
        return g_blockedNames;
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
