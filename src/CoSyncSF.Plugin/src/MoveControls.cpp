#include "MoveControls.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <cstring>
#include <format>
#include <mutex>

namespace cosync::plugin::movecontrols
{
    namespace
    {
        // ---- Address Library IDs (1.16.244, RE §A.10) ----
        constexpr std::uint64_t kManagerGlobalId = 938464;        // PlayerControls::Manager* (0x61E23E0)
        constexpr std::uint64_t kManagerVtableId = 433847;        // Manager primary vtable (0x4C43F70)
        constexpr std::uint64_t kPlayerControlsVtableId = 433843; // Manager+0x20 IMovementPlayerControls (0x4C43FA0)
        constexpr std::size_t kFillSlot = 1;                      // 0x12C5E60
        constexpr std::uint64_t kRegistryLookupId = 49356;        // 0x688B60 (registry, const BSFixedString*) -> void*
        constexpr std::uint64_t kRegistryAddId = 135280;          // 0x2732FD0 (registry, key, value) -> bool
        constexpr std::uint64_t kRegistryRemoveId = 135281;       // 0x27330A0 (registry, key)
        // Interface-name BSFixedString globals.
        constexpr std::uint64_t kKeyPlayerControls = 889677;       // "IMovementPlayerControls"
        constexpr std::uint64_t kKeyPlayerControlsFilter = 889678; // "IMovementPlayerControlsFilter"
        constexpr std::uint64_t kKeySetPlayerControls = 889679;    // "Set Player Controls"
        constexpr std::uint64_t kKeyMotionDriven = 932524;         // "IMovementMotionDrivenControl"
        constexpr std::size_t kControllerOffset = 0x240;          // Actor::movementController
        constexpr std::size_t kRegistryOffset = 0x48;             // MovementControllerAI interface registry
        constexpr std::size_t kManagerPlayerControlsOffset = 0x20;
        constexpr std::uint32_t kPlayerFormId = 0x14;
        constexpr std::uint64_t kControllerNpcVtableId = 452731;  // MovementControllerNPC primary vtable
        constexpr std::uint64_t kEnterState1Id = 103568;          // vfunc 9, 0x1AB7EB0
        constexpr std::size_t kEnterState1Slot = 9;
        constexpr std::size_t kControllerStateOffset = 0xC0;

        using Fill_t = std::int64_t (*)(void* self, std::uint32_t formId, void* out);
        using Lookup_t = void* (*)(void* registry, const void* key);
        using Add_t = bool (*)(void* registry, const void* key, void* value);
        using Remove_t = void (*)(void* registry, const void* key);

        Fill_t g_originalFill = nullptr;
        bool g_captureInstalled = false;
        std::mutex g_lock;
        Controls g_latest;

        std::uintptr_t Addr(std::uint64_t id) { return REL::Relocation<std::uintptr_t>{ REL::ID(id) }.address(); }
        const void* Key(std::uint64_t id) { return reinterpret_cast<const void*>(Addr(id)); }

        // Runs on whichever thread updates the player's movement controller.
        std::int64_t FillHook(void* self, std::uint32_t formId, void* out)
        {
            const auto r = g_originalFill(self, formId, out);
            if (formId == kPlayerFormId && out) {
                std::lock_guard lock(g_lock);
                std::memcpy(g_latest.data, out, kControlsSize);
                ++g_latest.seq;
            }
            return r;
        }

        // ---- CoSync IMovementPlayerControls (mirror experiment) ----
        // Vtable layout of the engine's interface: 0 dtor, 1 fill, 2/3 per-actor add/remove on the
        // manager (0x12C6350/0x12C6440). Ours: no-op for 0/2/3 (this object is static).
        void* MirrorDtor(void* self, int) { return self; }
        std::int64_t MirrorFill(void*, std::uint32_t, void* out)
        {
            if (!out) return 0;
            std::lock_guard lock(g_lock);
            if (g_latest.seq != 0) std::memcpy(out, g_latest.data, kControlsSize);
            return 0;
        }
        void MirrorNoop(void*, void*) {}

        // ---- Replay objects (network) ----
        struct ReplayObject
        {
            void** vtbl;
            std::uint32_t slot;
        };
        struct ReplayData
        {
            std::mutex lock;
            Controls controls;
            bool active = false;
            float turn = 0.0f;        // yaw delta (radians) to apply once, then 0 until the next update
            float heading = 0.0f;     // proxy heading after that delta
            bool moveSet = false;     // closed-loop movement overrides +0x08/+0x30
            float moveAngle = 0.0f;   // +0x08: direction relative to facing (atan2(right, forward))
            float moveMagnitude = 0.0f;  // +0x30: 0 = stand, 2 = run (observed)
            bool turnPending = false;
        };
        ReplayData g_replayData[kReplaySlots];
        std::int64_t ReplayFill(void* self, std::uint32_t, void* out)
        {
            if (!out) return 0;
            auto& d = g_replayData[static_cast<ReplayObject*>(self)->slot % kReplaySlots];
            std::lock_guard lock(d.lock);
            if (d.active) {
                std::memcpy(out, d.controls.data, kControlsSize);
                // +0x18 (turn rate while aligned to the camera) and +0x34 (raw look input) are
                // per-frame deltas on the sender. The receiver's engine reads this struct every frame
                // but a new one arrives only at 20 Hz, so replaying them over-turns the proxy
                // (in-game 2026-11-03: "mouse movement goes too far"). Heading comes from the
                // absolute +0x48 instead (it holds still when the sender does not turn).
                // Look: the sender's per-frame deltas are not replayed. Yaw is closed-loop instead:
                // the game thread computes the heading error to the remote player each frame and
                // the engine turns the proxy by it once (+0x18 = yaw delta, +0x48 = resulting
                // heading, as the engine's own producer writes them; RE A.10).
                auto* b = static_cast<std::byte*>(out);
                const float turn = d.turnPending ? d.turn : 0.0f;
                std::memcpy(b + 0x18, &turn, 4);
                std::memcpy(b + 0x48, &d.heading, 4);
                std::memset(b + 0x34, 0, 4);  // pitch input: not driven yet
                if (d.moveSet) {
                    std::memcpy(b + 0x08, &d.moveAngle, 4);
                    std::memcpy(b + 0x30, &d.moveMagnitude, 4);
                }
                std::memset(b + 0x40, 0, 4);
                d.turnPending = false;
            }
            return 0;
        }

        void* g_mirrorVtbl[] = { reinterpret_cast<void*>(&MirrorDtor), reinterpret_cast<void*>(&MirrorFill),
                                 reinterpret_cast<void*>(&MirrorNoop), reinterpret_cast<void*>(&MirrorNoop) };
        struct MirrorObject
        {
            void** vtbl = g_mirrorVtbl;
            std::byte pad[0x40]{};
        } g_mirror;

        void* g_replayVtbl[] = { reinterpret_cast<void*>(&MirrorDtor), reinterpret_cast<void*>(&ReplayFill),
                                 reinterpret_cast<void*>(&MirrorNoop), reinterpret_cast<void*>(&MirrorNoop) };
        ReplayObject g_replay[kReplaySlots]{};  // vtables set by InitReplay() (main thread, before first attach)
        bool g_replayInit = false;
        void InitReplay()
        {
            if (g_replayInit) return;
            for (std::uint32_t i = 0; i < kReplaySlots; ++i) g_replay[i] = ReplayObject{ g_replayVtbl, i };
            g_replayInit = true;
        }

        void* Registry(std::uint32_t refId, std::string& why)
        {
            auto* form = RE::TESForm::LookupByID(refId);
            if (!form || !form->Is(RE::FormType::kACHR)) {
                why = "no actor";
                return nullptr;
            }
            auto* controller = *reinterpret_cast<std::byte**>(reinterpret_cast<std::byte*>(form) + kControllerOffset);
            if (!controller) {
                why = "no movement controller";
                return nullptr;
            }
            auto* registry = *reinterpret_cast<void**>(controller + kRegistryOffset);
            if (!registry) why = "no interface registry";
            return registry;
        }

        void* ManagerPlayerControls()
        {
            auto* mgr = *reinterpret_cast<std::byte**>(Addr(kManagerGlobalId));
            if (!mgr) return nullptr;
            if (*reinterpret_cast<std::uintptr_t*>(mgr) != Addr(kManagerVtableId)) return nullptr;
            auto* iface = mgr + kManagerPlayerControlsOffset;
            if (*reinterpret_cast<std::uintptr_t*>(iface) != Addr(kPlayerControlsVtableId)) return nullptr;
            return iface;
        }

        std::string Tag(void* p)
        {
            if (!p) return "-";
            if (p == &g_mirror) return "CoSync-mirror";
            if (p >= static_cast<void*>(&g_replay[0]) && p < static_cast<void*>(&g_replay[kReplaySlots]))
                return std::format("CoSync-replay#{}", static_cast<ReplayObject*>(p)->slot);
            if (p == ManagerPlayerControls()) return "PlayerControls::Manager";
            return std::format("{:#x}", reinterpret_cast<std::uintptr_t>(p));
        }
    }

    bool InstallCapture(std::string& detail)
    {
        if (g_captureInstalled) {
            detail = "player-controls capture already installed";
            return true;
        }
        REL::Relocation<std::uintptr_t> vtbl{ REL::ID(kPlayerControlsVtableId) };
        g_originalFill = reinterpret_cast<Fill_t>(vtbl.write_vfunc(kFillSlot, &FillHook));
        g_captureInstalled = true;
        detail = std::format("player-controls capture installed (vtable ID {} slot {})", kPlayerControlsVtableId,
                             kFillSlot);
        return true;
    }

    Controls LatestPlayerControls()
    {
        std::lock_guard lock(g_lock);
        return g_latest;
    }

    std::string Describe(const Controls& c)
    {
        if (c.seq == 0) return "none captured";
        float f[0x58 / 4];
        std::memcpy(f, c.data, sizeof(f));
        std::string all;
        for (int i = 0; i < 21; ++i) all += std::format("{}{:02X}={:.3f}", i ? " " : "", i * 4, f[i]);
        return std::format("#{} {} b54={}", c.seq, all, static_cast<int>(static_cast<unsigned char>(c.data[0x54])));
    }

    namespace
    {
        bool Replace(std::uint32_t refId, void* object, std::string& detail)
        {
            std::string why;
            auto* registry = Registry(refId, why);
            if (!registry) {
                detail = why;
                return false;
            }
            static REL::Relocation<Lookup_t> lookup{ REL::ID(kRegistryLookupId) };
            static REL::Relocation<Add_t> add{ REL::ID(kRegistryAddId) };
            static REL::Relocation<Remove_t> remove{ REL::ID(kRegistryRemoveId) };
            const void* key = Key(kKeyPlayerControls);
            void* before = lookup(registry, key);
            int removed = 0;
            while (removed < 4 && lookup(registry, key)) {
                remove(registry, key);
                ++removed;
            }
            const bool ok = add(registry, key, object);
            void* after = lookup(registry, key);
            detail = std::format("IMovementPlayerControls {} -> {} (removed {}, add {})", Tag(before), Tag(after),
                                 removed, ok ? "ok" : "FAILED");
            return ok && after == object;
        }
    }

    bool AttachReplay(std::uint32_t refId, std::uint32_t slotId, std::string& detail)
    {
        InitReplay();
        return Replace(refId, &g_replay[slotId % kReplaySlots], detail);
    }

    void SetReplay(std::uint32_t slotId, const Controls& c, bool active)
    {
        auto& d = g_replayData[slotId % kReplaySlots];
        std::lock_guard lock(d.lock);
        d.controls = c;
        d.active = active;
    }

    void SetReplayTurn(std::uint32_t slotId, float yawDelta, float heading)
    {
        auto& d = g_replayData[slotId % kReplaySlots];
        std::lock_guard lock(d.lock);
        d.turn = yawDelta;
        d.heading = heading;
        d.turnPending = true;
    }

    namespace
    {
        // Actor vfunc 363 (0x1983620): NPCs may jump only with a CombatController (Actor+0x250)
        // whose byte +0x1E4 is clear; the player overrides it (0x1A789E0). A proxy is not in combat,
        // so 0x18E53E0 refuses (2026-11-03: combatController null). The hook answers true only for
        // the actor CoSync is jumping right now, on this thread, during that call.
        using CanJump_t = bool (*)(RE::Actor*);
        CanJump_t g_originalCanJump = nullptr;
        thread_local RE::Actor* t_jumping = nullptr;
        bool CanJumpHook(RE::Actor* a) { return a == t_jumping || g_originalCanJump(a); }
    }

    void InstallJumpHook(std::string& detail)
    {
        if (g_originalCanJump) {
            detail = "proxy jump hook already installed";
            return;
        }
        REL::Relocation<std::uintptr_t> actorVtbl{ REL::ID(451614) };  // Actor primary vtable
        // Original first, then patch: the hook must never run with a null original.
        g_originalCanJump = reinterpret_cast<CanJump_t>(reinterpret_cast<void* const*>(actorVtbl.address())[363]);
        actorVtbl.write_vfunc(363, &CanJumpHook);
        detail = "proxy jump hook installed (Actor vtable ID 451614 slot 363)";
    }

    bool Jump(std::uint32_t refId, std::string* detail)
    {
        if (!g_originalCanJump) return false;  // InstallJumpHook not called
        auto* form = RE::TESForm::LookupByID(refId);
        if (!form || !form->Is(RE::FormType::kACHR)) return false;
        auto* actor = static_cast<RE::Actor*>(form);
        auto* base = reinterpret_cast<std::byte*>(actor);
        using Fn = bool (*)(RE::Actor*, float);
        static REL::Relocation<Fn> jump{ REL::ID(100537) };
        static const float height = *reinterpret_cast<const float*>(Addr(500287));
        if (detail) {
            // Preconditions of 0x18E53E0: mobility 0x197AA70 != 2; character controller at
            // process->+0x08->+0x458; vfunc 363 (vtbl+0xB58) true; process+0xEB == 0;
            // Actor+0x250 -> +0x1E5 == 0.
            using Mob_t = std::uint8_t (*)(RE::Actor*);
            static REL::Relocation<Mob_t> mobility{ REL::ID(101635) };
            auto* process = *reinterpret_cast<std::byte**>(base + 0x228);
            std::byte* mid = process ? *reinterpret_cast<std::byte**>(process + 0x08) : nullptr;
            void* cc = mid ? *reinterpret_cast<void**>(mid + 0x458) : nullptr;
            const auto vtbl = *reinterpret_cast<void* const* const*>(actor);
            const bool v363 = reinterpret_cast<bool (*)(RE::Actor*)>(vtbl[363])(actor);
            const int p235 = process ? static_cast<int>(*reinterpret_cast<std::uint8_t*>(process + 0xEB)) : -1;
            auto* a250 = *reinterpret_cast<std::byte**>(base + 0x250);
            const int a485 = a250 ? static_cast<int>(*reinterpret_cast<std::uint8_t*>(a250 + 0x1E5)) : -1;
            *detail = std::format("mobility={} charController={} vfunc363={} proc+EB={} a250+1E5={}",
                                  static_cast<int>(mobility(actor)), cc ? "yes" : "NONE", v363, p235, a485);
        }
        t_jumping = actor;
        const bool ok = jump(actor, height);
        t_jumping = nullptr;
        return ok;
    }

    void SetReplayMove(std::uint32_t slotId, float angle, float magnitude)
    {
        auto& d = g_replayData[slotId % kReplaySlots];
        std::lock_guard lock(d.lock);
        d.moveSet = true;
        d.moveAngle = angle;
        d.moveMagnitude = magnitude;
    }

    bool SetAim(std::uint32_t refId, float pitch, bool aimActive)
    {
        auto* form = RE::TESForm::LookupByID(refId);
        if (!form || !form->Is(RE::FormType::kACHR)) return false;
        auto* process = *reinterpret_cast<std::byte**>(reinterpret_cast<std::byte*>(form) + 0x228);
        if (!process) return false;
        auto* mid = *reinterpret_cast<std::byte**>(process + 0x08);
        if (!mid) return false;
        *reinterpret_cast<float*>(mid + 0x52C) = 0.0f;   // DirectAtHeadingVar
        *reinterpret_cast<float*>(mid + 0x530) = pitch;  // DirectAtPitchVar
        *reinterpret_cast<std::uint8_t*>(mid + 0x587) = aimActive ? 1 : 0;  // bAimActive
        return true;
    }

    int ControllerState(std::uint32_t refId)
    {
        auto* form = RE::TESForm::LookupByID(refId);
        if (!form || !form->Is(RE::FormType::kACHR)) return -1;
        auto* controller = *reinterpret_cast<std::byte**>(reinterpret_cast<std::byte*>(form) + kControllerOffset);
        return controller ? *reinterpret_cast<std::int32_t*>(controller + kControllerStateOffset) : -1;
    }

    bool AttachMirror(std::uint32_t refId, std::string& detail) { return Replace(refId, &g_mirror, detail); }

    bool EnterPlayerControlsState(std::uint32_t refId, std::string& detail)
    {
        auto* form = RE::TESForm::LookupByID(refId);
        if (!form || !form->Is(RE::FormType::kACHR)) {
            detail = "no actor";
            return false;
        }
        auto* controller = *reinterpret_cast<std::byte**>(reinterpret_cast<std::byte*>(form) + kControllerOffset);
        if (!controller) {
            detail = "no movement controller";
            return false;
        }
        const auto vtbl = *reinterpret_cast<std::uintptr_t* const*>(controller);
        if (reinterpret_cast<std::uintptr_t>(vtbl) != Addr(kControllerNpcVtableId) ||
            vtbl[kEnterState1Slot] != Addr(kEnterState1Id)) {
            detail = "controller vtable is not MovementControllerNPC's; refused";
            return false;
        }
        auto state = [&] { return *reinterpret_cast<std::int32_t*>(controller + kControllerStateOffset); };
        const auto before = state();
        using Fn = std::uint8_t (*)(void*);
        const auto ok = reinterpret_cast<Fn>(vtbl[kEnterState1Slot])(controller);
        detail = std::format("vfunc 9 returned {}; state {} -> {}", ok, before, state());
        return ok != 0;
    }

    std::string DescribeRegistry(std::uint32_t refId)
    {
        std::string why;
        auto* registry = Registry(refId, why);
        if (!registry) return why;
        static REL::Relocation<Lookup_t> lookup{ REL::ID(kRegistryLookupId) };
        return std::format("registry: PlayerControls={} Filter={} SetPlayerControls={} MotionDriven={}",
                           Tag(lookup(registry, Key(kKeyPlayerControls))),
                           Tag(lookup(registry, Key(kKeyPlayerControlsFilter))),
                           Tag(lookup(registry, Key(kKeySetPlayerControls))),
                           Tag(lookup(registry, Key(kKeyMotionDriven))));
    }
}
