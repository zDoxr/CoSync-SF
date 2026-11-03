#include "AnimProbe.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>

namespace cosync::plugin::animprobe
{
    namespace
    {
        // ---- Verified layout (docs/ANIMATION_REVERSE_ENGINEERING.md) ----
        constexpr std::size_t kProcessOffset = 0x228;     // Actor::currentProcess (RE A.5, RE §5)
        constexpr std::size_t kControllerOffset = 0x240;  // Actor::movementController (RE A.5)
        constexpr std::size_t kHolderOffset = 0x60;       // IAnimationGraphManagerHolder (RE A.1)
        constexpr std::size_t kProcessByteE7 = 0xE7;      // motion-driven iff 0 (RE A.5)
        constexpr std::size_t kCtrlMode = 0x5C;           // 1/2 (RE A.4)
        constexpr std::size_t kCtrlConfig = 0xC0;         // configuration state 0..13 (RE A.6)
        constexpr std::size_t kCtrlByte70 = 0x70;         // written by 0x2738D90 (RE A.4)
        // Holder getters. Float: VERIFIED (0x18EECF0 calls vtbl+0x90 for "Speed").
        // Int/bool: CommonLibSF slot numbers, checked at runtime by SelfCheckPlayer().
        constexpr std::size_t kGetFloatSlot = 0x12;
        constexpr std::size_t kGetIntSlot = 0x13;
        constexpr std::size_t kGetBoolSlot = 0x14;
        constexpr std::uint64_t kPlayerCharacterVtableId = 452447;  // RVA 0x4CBC448
        constexpr std::uint64_t kReinitMovementControllerId = 100903;  // RVA 0x1922740

        const std::vector<VarSpec> kVars = {
            // Locomotion (engine-cached each frame, RE A.3)
            { "Speed", Kind::kFloat },
            { "Direction", Kind::kFloat },
            { "TurnDelta", Kind::kFloat },
            { "SpeedSmoothed", Kind::kFloat },
            { "DirectionSmoothed", Kind::kFloat },
            { "VelocityZ", Kind::kFloat },
            // Aim/look pitch candidates (which ones carry pitch: to be measured)
            { "Pitch", Kind::kFloat },
            { "PitchDelta", Kind::kFloat },
            { "fCameraPitch", Kind::kFloat },
            { "DirectAtPitchVar", Kind::kFloat },
            { "DirectAtHeadingVar", Kind::kFloat },
            { "LookAtPitchVar", Kind::kFloat },
            // Identity / mode (set at graph init, RE A.2)
            { "IsPlayer", Kind::kBool },
            { "IsNPC", Kind::kBool },
            { "iIsPlayer", Kind::kInt },
            { "IsFirstPerson", Kind::kBool },
            { "bIsPathing", Kind::kBool },
            { "bGraphDriven", Kind::kBool },
            { "IsUsingCodeDrivenRotation", Kind::kBool },
            // Locomotion sync states
            { "iSyncIdleLocomotion", Kind::kInt },
            { "iSyncForwardState", Kind::kInt },
            { "iSyncStrafeState", Kind::kInt },
            { "iSyncTurnState", Kind::kInt },
            { "iSyncSprintState", Kind::kInt },
            { "iSyncJumpState", Kind::kInt },
            { "iSyncGravity", Kind::kInt },
            { "iIsInSneak", Kind::kInt },
            { "bIsSliding", Kind::kInt },
            { "iLadderClimbState", Kind::kInt },
            // Weapon / combat (bound to process memory, RE A.4)
            { "iSyncWeaponDrawState", Kind::kInt },
            { "iSyncActorStance", Kind::kInt },
            { "iSyncRelaxedReadyAlert", Kind::kInt },
            { "iIsSighted", Kind::kInt },
            { "iSightedRequested", Kind::kInt },
            { "iSyncSighted", Kind::kInt },
            { "iSyncGunDown", Kind::kInt },
            { "iSyncGunActions", Kind::kInt },
            { "iAttackState", Kind::kInt },
            { "bAimActive", Kind::kBool },
            { "bIsReloading", Kind::kBool },
            { "bIsEquipping", Kind::kBool },
        };

        // ---- SEH-guarded primitives (no destructible locals allowed in these) ----
        bool SafeCopy(const void* src, void* dst, std::size_t n) noexcept
        {
            __try {
                std::memcpy(dst, src, n);
                return true;
            } __except (1) {
                return false;
            }
        }

        template <class T>
        bool CallGetter(void* holder, std::size_t slot, const RE::BSFixedString* name, T* out) noexcept
        {
            using Fn = bool (*)(void*, const RE::BSFixedString&, T&);
            __try {
                const auto vtbl = *reinterpret_cast<void* const* const*>(holder);
                return reinterpret_cast<Fn>(vtbl[slot])(holder, *name, *out);
            } __except (1) {
                return false;
            }
        }

        const std::vector<RE::BSFixedString>& Names()
        {
            static const std::vector<RE::BSFixedString> names = [] {
                std::vector<RE::BSFixedString> v;
                v.reserve(kVars.size());
                for (const auto& s : kVars) v.emplace_back(s.name);
                return v;
            }();
            return names;
        }

        RE::Actor* LookupActor(std::uint32_t refId)
        {
            auto* form = RE::TESForm::LookupByID(refId);
            if (!form || !form->Is(RE::FormType::kACHR)) return nullptr;
            return static_cast<RE::Actor*>(form);
        }

        std::string Value(const VarSpec& spec, const VarValue& v)
        {
            if (!v.ok) return "-";
            switch (spec.kind) {
            case Kind::kFloat: return std::format("{:.2f}", v.f);
            case Kind::kInt: return std::to_string(v.i);
            case Kind::kBool: return v.b ? "1" : "0";
            }
            return "?";
        }

        bool SameDiscrete(const VarSpec& spec, const VarValue& a, const VarValue& b)
        {
            if (a.ok != b.ok) return false;
            if (!a.ok) return true;
            return spec.kind == Kind::kInt ? a.i == b.i : a.b == b.b;
        }
    }

    const std::vector<VarSpec>& Vars() { return kVars; }

    Snapshot Read(std::uint32_t refId)
    {
        Snapshot s;
        s.values.resize(kVars.size());
        auto* actor = LookupActor(refId);
        if (!actor) return s;
        s.found = true;
        auto* base = reinterpret_cast<std::byte*>(actor);

        static const std::uintptr_t playerVtbl = REL::Relocation<std::uintptr_t>{ REL::ID(kPlayerCharacterVtableId) }.address();
        std::uintptr_t vtbl = 0;
        s.readable &= SafeCopy(base, &vtbl, sizeof(vtbl));
        s.isPlayerClass = vtbl == playerVtbl;

        s.readable &= SafeCopy(base + kProcessOffset, &s.process, sizeof(s.process));
        s.readable &= SafeCopy(base + kControllerOffset, &s.controller, sizeof(s.controller));
        std::uint8_t b8 = 0;
        if (s.process && SafeCopy(reinterpret_cast<std::byte*>(s.process) + kProcessByteE7, &b8, 1)) s.processByteE7 = b8;
        if (s.controller) {
            auto* c = reinterpret_cast<std::byte*>(s.controller);
            std::int32_t i32 = 0;
            if (SafeCopy(c + kCtrlMode, &b8, 1)) s.controllerMode = b8;
            if (SafeCopy(c + kCtrlByte70, &b8, 1)) s.controllerByte70 = b8;
            if (SafeCopy(c + kCtrlConfig, &i32, 4)) s.controllerConfig = i32;
        }

        void* holder = base + kHolderOffset;
        const auto& names = Names();
        for (std::size_t k = 0; k < kVars.size(); ++k) {
            auto& v = s.values[k];
            switch (kVars[k].kind) {
            case Kind::kFloat: v.ok = CallGetter(holder, kGetFloatSlot, &names[k], &v.f); break;
            case Kind::kInt: v.ok = CallGetter(holder, kGetIntSlot, &names[k], &v.i); break;
            case Kind::kBool: v.ok = CallGetter(holder, kGetBoolSlot, &names[k], &v.b); break;
            }
            s.graphPresent |= v.ok;
        }
        return s;
    }

    bool ReadFloat(std::uint32_t refId, const char* name, float& out)
    {
        auto* actor = LookupActor(refId);
        if (!actor) return false;
        const RE::BSFixedString n(name);
        return CallGetter(reinterpret_cast<std::byte*>(actor) + kHolderOffset, kGetFloatSlot, &n, &out);
    }

    std::string DescribeState(const Snapshot& s)
    {
        if (!s.found) return "no actor";
        std::string out = std::format("{} ctrl={} mode={} cfg={} b70={} procE7={}{}{} |",
                                      s.isPlayerClass ? "PlayerCharacter" : "Actor",
                                      s.controller ? "yes" : "NONE", s.controllerMode, s.controllerConfig,
                                      s.controllerByte70, s.processByteE7, s.readable ? "" : " (READ FAULT)",
                                      s.graphPresent ? "" : " graph=NONE");
        for (std::size_t k = 0; k < kVars.size(); ++k) {
            if (kVars[k].kind == Kind::kFloat || !s.values[k].ok) continue;
            out += std::format(" {}={}", kVars[k].name, Value(kVars[k], s.values[k]));
        }
        return out;
    }

    std::string DescribeFloats(const Snapshot& s)
    {
        std::string out;
        for (std::size_t k = 0; k < kVars.size(); ++k) {
            if (kVars[k].kind != Kind::kFloat) continue;
            out += std::format("{}{}={}", out.empty() ? "" : " ", kVars[k].name, Value(kVars[k], s.values[k]));
        }
        return out;
    }

    std::string DescribeTransitions(const Snapshot& a, const Snapshot& b)
    {
        std::string out;
        auto add = [&](std::string_view what, auto from, auto to) {
            out += std::format("{}{} {}->{}", out.empty() ? "" : ", ", what, from, to);
        };
        if (a.found != b.found) add("actor", a.found, b.found);
        if (a.controller != b.controller) add("controller", a.controller ? "yes" : "none", b.controller ? "yes" : "none");
        if (a.controllerMode != b.controllerMode) add("mode", a.controllerMode, b.controllerMode);
        if (a.controllerConfig != b.controllerConfig) add("cfg", a.controllerConfig, b.controllerConfig);
        if (a.controllerByte70 != b.controllerByte70) add("b70", a.controllerByte70, b.controllerByte70);
        if (a.processByteE7 != b.processByteE7) add("procE7", a.processByteE7, b.processByteE7);
        if (a.values.size() == b.values.size()) {
            for (std::size_t k = 0; k < kVars.size(); ++k) {
                if (kVars[k].kind == Kind::kFloat || SameDiscrete(kVars[k], a.values[k], b.values[k])) continue;
                add(kVars[k].name, Value(kVars[k], a.values[k]), Value(kVars[k], b.values[k]));
            }
        }
        return out;
    }

    bool FloatsChanged(const Snapshot& a, const Snapshot& b, float eps)
    {
        if (a.values.size() != b.values.size()) return true;
        for (std::size_t k = 0; k < kVars.size(); ++k) {
            if (kVars[k].kind != Kind::kFloat) continue;
            if (a.values[k].ok != b.values[k].ok) return true;
            if (a.values[k].ok && std::abs(a.values[k].f - b.values[k].f) > eps) return true;
        }
        return false;
    }

    std::string SelfCheckPlayer(const Snapshot& p, bool& passed)
    {
        auto find = [&](std::string_view n) -> const VarValue* {
            for (std::size_t k = 0; k < kVars.size(); ++k)
                if (kVars[k].name == n) return &p.values[k];
            return nullptr;
        };
        const auto* isPlayer = find("IsPlayer");
        const auto* isNpc = find("IsNPC");
        const auto* iIsPlayer = find("iIsPlayer");
        const bool boolOk = isPlayer && isPlayer->ok && isPlayer->b && isNpc && isNpc->ok && !isNpc->b;
        const bool intOk = iIsPlayer && iIsPlayer->ok && iIsPlayer->i == 1;
        passed = p.isPlayerClass && boolOk && intOk;
        return std::format("getter self-check on the player: class={} bool getter(slot 0x14) {} [IsPlayer={} IsNPC={}], "
                           "int getter(slot 0x13) {} [iIsPlayer={}]",
                           p.isPlayerClass ? "PlayerCharacter" : "NOT PlayerCharacter", boolOk ? "OK" : "FAILED",
                           isPlayer ? Value({ "", Kind::kBool }, *isPlayer) : "?",
                           isNpc ? Value({ "", Kind::kBool }, *isNpc) : "?", intOk ? "OK" : "FAILED",
                           iIsPlayer ? Value({ "", Kind::kInt }, *iIsPlayer) : "?");
    }

    bool ReinitMovementController(std::uint32_t refId, bool motionDriven, std::string& detail)
    {
        auto* actor = LookupActor(refId);
        if (!actor) {
            detail = "no actor";
            return false;
        }
        using Fn = std::int64_t (*)(RE::Actor*, bool);
        static REL::Relocation<Fn> fn{ REL::ID(kReinitMovementControllerId) };
        const auto before = Read(refId);
        fn(actor, motionDriven);
        const auto after = Read(refId);
        detail = std::format("0x1922740(actor, {}): mode {}->{} cfg {}->{}", motionDriven, before.controllerMode,
                             after.controllerMode, before.controllerConfig, after.controllerConfig);
        return true;
    }
}
