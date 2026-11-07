#include "CrowdSync.h"

#include "cosync/shared/NpcReplication.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <atomic>
#include <format>
#include <string>
#include <shared_mutex>
#include <unordered_map>

namespace cosync::plugin::crowd
{
    namespace
    {
        constexpr std::uint64_t kFsmCallerId = 81606;          // 0x127E900, one crowd actor's update
        constexpr std::uint64_t kFsmId = 81607;                // 0x127EBA0, its state machine
        constexpr std::uint64_t kSetTargetId = 81549;          // 0x1271B20
        constexpr std::uint64_t kSetStateId = 81547;           // 0x12718E0
        constexpr std::uint64_t kInhibitThresholdId = 914896;  // 0x5F107B0, uCrowdInhibitThresholdMinActor
        constexpr std::size_t kFsmCallOffset = 0x1EA;          // 0x127EAEA, the only caller
        // 0x1275B30, CrowdManager's update, calls 0x1276A70 once per pending spawn, at these two places.
        constexpr std::uint64_t kUpdateId = 81575;
        constexpr std::uint64_t kSpawnId = 81577;
        constexpr std::size_t kSpawnCallOffsets[] = { 0x5EE, 0x64A };
        constexpr std::size_t kTargetOffset = 0x04;
        constexpr std::size_t kStateOffset = 0x78;

        // The fourth argument is what Hex-Rays reads into xmm3. The caller never sets it, so it is
        // passed along untouched rather than assumed unused.
        using Fsm_t = void (*)(void* manager, std::byte* entry, RE::Actor* actor, double passthrough);
        using SetTarget_t = void (*)(void* manager, std::byte* entry, std::uint32_t target);
        using SetState_t = void (*)(std::byte* entry, std::uint32_t state);

        Fsm_t g_fsm = nullptr;
        std::shared_mutex g_lock;
        std::unordered_map<std::uint32_t, std::uint32_t> g_orders;
        std::vector<Order> g_lastOrders;
        std::atomic<bool> g_haveOrders{ false };
        std::atomic<std::uint64_t> g_retargets{ 0 };
        std::atomic<std::uint64_t> g_holds{ 0 };
        std::uint32_t g_savedThreshold = 0;
        bool g_inhibitionOff = false;

        // Runs on job threads, once per crowd actor per update.
        void FsmHook(void* manager, std::byte* entry, RE::Actor* actor, double passthrough)
        {
            if (g_haveOrders.load(std::memory_order_acquire)) {
                std::uint32_t wanted = 0;
                {
                    std::shared_lock lock(g_lock);
                    if (const auto it = g_orders.find(actor->GetFormID()); it != g_orders.end()) wanted = it->second;
                }
                if (wanted != 0) {
                    const auto current = *reinterpret_cast<const std::uint32_t*>(entry + kTargetOffset);
                    const auto state = *reinterpret_cast<const std::uint32_t*>(entry + kStateOffset);
                    switch (npc::DecideCrowdStep(state, current == wanted)) {
                    case npc::CrowdStep::Hold:
                        g_holds.fetch_add(1, std::memory_order_relaxed);
                        return;
                    case npc::CrowdStep::Retarget: {
                        // The same two calls as state 8 of the machine, with the authority's choice.
                        static REL::Relocation<SetTarget_t> setTarget{ REL::ID(kSetTargetId) };
                        static REL::Relocation<SetState_t> setState{ REL::ID(kSetStateId) };
                        setTarget(manager, entry, wanted);
                        setState(entry, 0);
                        g_retargets.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    case npc::CrowdStep::Run:
                        break;
                    }
                }
            }
            g_fsm(manager, entry, actor, passthrough);
        }
    }

    namespace
    {
        using Spawn_t = void (*)(void*, void*, std::int32_t, void*, void*);
        Spawn_t g_spawn = nullptr;
        std::atomic<bool> g_blockSpawns{ false };
        std::atomic<std::uint64_t> g_blocked{ 0 };

        void SpawnHook(void* manager, void* request, std::int32_t flag, void* a4, void* a5)
        {
            if (g_blockSpawns.load(std::memory_order_relaxed)) {
                g_blocked.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            g_spawn(manager, request, flag, a4, a5);
        }

        std::string InstallSpawnHook()
        {
            const REL::Relocation<std::uintptr_t> update{ REL::ID(kUpdateId) };
            const REL::Relocation<std::uintptr_t> spawn{ REL::ID(kSpawnId) };
            for (const auto offset : kSpawnCallOffsets) {
                const auto* code = reinterpret_cast<const std::uint8_t*>(update.address() + offset);
                if (code[0] != 0xE8 || update.address() + offset + 5 + *reinterpret_cast<const std::int32_t*>(code + 1) != spawn.address()) {
                    return std::format("spawn hook refused: no call to the spawn at +{:#x}", offset);
                }
            }
            for (const auto offset : kSpawnCallOffsets) {
                REL::Relocation<std::uintptr_t> site{ update.address() + offset };
                g_spawn = reinterpret_cast<Spawn_t>(site.write_call<5>(&SpawnHook));
            }
            return "spawn hook installed";
        }
    }

    void SetSpawnBlocked(bool blocked) noexcept { g_blockSpawns.store(blocked && g_spawn != nullptr, std::memory_order_relaxed); }
    std::uint64_t TakeBlockedSpawns() noexcept { return g_blocked.exchange(0, std::memory_order_relaxed); }

    bool Install(std::string& detail)
    {
        if (g_fsm) {
            detail = "crowd hook already installed";
            return true;
        }
        const REL::Relocation<std::uintptr_t> caller{ REL::ID(kFsmCallerId) };
        const REL::Relocation<std::uintptr_t> fsm{ REL::ID(kFsmId) };
        const auto call = caller.address() + kFsmCallOffset;
        const auto* code = reinterpret_cast<const std::uint8_t*>(call);
        if (code[0] != 0xE8) {
            detail = "crowd hook refused: no call at the expected place";
            return false;
        }
        const auto relative = *reinterpret_cast<const std::int32_t*>(code + 1);
        if (call + 5 + relative != fsm.address()) {
            detail = "crowd hook refused: the call goes somewhere else";
            return false;
        }
        REL::Relocation<std::uintptr_t> site{ call };
        g_fsm = reinterpret_cast<Fsm_t>(site.write_call<5>(&FsmHook));
        detail = "crowd hook installed; " + InstallSpawnHook();
        return true;
    }

    bool Installed() noexcept { return g_fsm != nullptr; }

    void SetOrders(const std::vector<Order>& orders)
    {
        if (orders == g_lastOrders) return;
        g_lastOrders = orders;
        std::unique_lock lock(g_lock);
        g_orders.clear();
        for (const auto& o : orders) g_orders[o.actor] = o.target;
        g_haveOrders.store(!g_orders.empty(), std::memory_order_release);
    }

    void SetInhibitionOff(bool off)
    {
        if (off == g_inhibitionOff) return;
        // 0x1275360 picks strategy (crowd size > this) + 1 on the main thread every update, and
        // Disable, the first, clears the flag on every entry itself. No count reaches this.
        constexpr std::uint32_t kNever = 0xFFFFFFFF;
        REL::Relocation<std::uint32_t*> threshold{ REL::ID(kInhibitThresholdId) };
        if (off) {
            g_savedThreshold = *threshold;
            *threshold = kNever;
        } else {
            *threshold = g_savedThreshold;
        }
        g_inhibitionOff = off;
    }

    Counters TakeCounters() noexcept
    {
        return { g_retargets.exchange(0, std::memory_order_relaxed), g_holds.exchange(0, std::memory_order_relaxed) };
    }
}
