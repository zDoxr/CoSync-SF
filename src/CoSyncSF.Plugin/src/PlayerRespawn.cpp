#include "PlayerRespawn.h"

#include "Avatars.h"
#include "DevHarness.h"
#include "NpcReplication.h"

#include "cosync/shared/Log.h"

#include <RE/Starfield.h>
#include <REL/Relocation.h>

#include <MinHook.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <optional>

namespace cosync::plugin::respawn
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;

        constexpr std::uint64_t kKillId = 100769;           // 0x1906010, every engine kill goes through it
        // Lethal damage does not pass the dispatcher: it calls the actor's own death slot (seen in game).
        // Both slots are the PlayerCharacter table's, so no other actor is touched.
        constexpr std::uint64_t kPlayerVtableId = 452447;   // 0x4CBC448
        constexpr std::size_t kDeathSlot = 386;             // what the dispatcher calls, vtable +3088
        constexpr std::size_t kLifeStateSlot = 367;         // sets the life state, vtable +0xB78
        constexpr std::uint32_t kLifeDying = 1;
        constexpr std::uint32_t kLifeDead = 2;
        constexpr std::uint64_t kCanSaveId = 98657;         // 0x1846EF0, asked by the menus, quick save and autosave
        constexpr std::uint64_t kSaveManagerId = 883588;    // 0x5E6D6E8
        constexpr std::uint64_t kInSpaceId = 63482;         // 0xB58D50, ObjectReference.IsInSpace
        constexpr std::size_t kPlayerFlagsOffset = 0xF64;   // bit 0: saving disabled, as Game.SetInChargen writes it
        constexpr std::uint8_t kSavingDisabled = 1;
        constexpr std::size_t kLifeStateOffset = 0xF8;
        constexpr std::uint32_t kLifeStateMask = 0x1E0000;
        constexpr std::uint32_t kDeadState1 = 0x20000;
        constexpr std::uint32_t kDeadState2 = 0x40000;

        constexpr std::uint32_t kMarkerBaseId = 0x3B;  // XMarker, what the checkpoint is placed as
        constexpr auto kCheckpointEvery = 4s;
        constexpr float kCheckpointMoved = 3.0f;      // meters
        constexpr float kSafeHealthPercent = 40.0f;
        constexpr float kFallingPerCheck = 3.0f;      // meters of height change between checks
        constexpr auto kGrace = 5s;                   // after returning, hits cannot bring the player down again
        constexpr auto kGraceTopUp = 250ms;
        constexpr auto kVerifyAfter = 3s;

        using Kill_t = std::int64_t (*)(RE::Actor*, RE::Actor*, float, std::uint8_t, char, char, char);
        Kill_t g_kill = nullptr;
        std::atomic<bool> g_active{ false };
        std::atomic<std::uint32_t> g_caught{ 0 };

        // Runs on whichever thread applied the blow, so it only counts: the rest happens in Tick.
        std::int64_t KillHook(RE::Actor* actor, RE::Actor* killer, float amount, std::uint8_t a4, char a5, char a6, char a7)
        {
            if (g_active.load(std::memory_order_relaxed) && actor == static_cast<RE::Actor*>(RE::PlayerCharacter::GetSingleton())) {
                g_caught.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            return g_kill(actor, killer, amount, a4, a5, a6, a7);
        }

        using Death_t = std::int64_t (*)(RE::Actor*, RE::Actor*, float, std::uint8_t, char, char);
        Death_t g_death = nullptr;
        std::int64_t DeathHook(RE::Actor* actor, RE::Actor* killer, float amount, std::uint8_t a4, char a5, char a6)
        {
            if (g_active.load(std::memory_order_relaxed)) {
                g_caught.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            return g_death(actor, killer, amount, a4, a5, a6);
        }

        using SetLife_t = void (*)(RE::Actor*, std::uint32_t, std::int64_t);
        SetLife_t g_setLife = nullptr;
        void SetLifeHook(RE::Actor* actor, std::uint32_t state, std::int64_t extra)
        {
            if (g_active.load(std::memory_order_relaxed) && (state == kLifeDying || state == kLifeDead)) {
                g_caught.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            g_setLife(actor, state, extra);
        }

        std::byte* PlayerBytes()
        {
            return reinterpret_cast<std::byte*>(RE::PlayerCharacter::GetSingleton());
        }

        bool SavingBlockedBit()
        {
            auto* player = PlayerBytes();
            return player && (*reinterpret_cast<std::uint8_t*>(player + kPlayerFlagsOffset) & kSavingDisabled) != 0;
        }

        void WriteSavingBlockedBit(bool blocked)
        {
            auto* player = PlayerBytes();
            if (!player) return;
            auto& flags = *reinterpret_cast<std::uint8_t*>(player + kPlayerFlagsOffset);
            flags = blocked ? static_cast<std::uint8_t>(flags | kSavingDisabled) : static_cast<std::uint8_t>(flags & ~kSavingDisabled);
        }

        std::optional<bool> EngineAllowsSaving()
        {
            static REL::Relocation<void**> manager{ REL::ID(kSaveManagerId) };
            static REL::Relocation<bool (*)(void*, bool)> canSave{ REL::ID(kCanSaveId) };
            if (!*manager) return std::nullopt;
            return canSave(*manager, false);
        }

        std::uint32_t LifeStateBits()
        {
            auto* player = PlayerBytes();
            return player ? *reinterpret_cast<std::uint32_t*>(player + kLifeStateOffset) & kLifeStateMask : 0;
        }

        bool InSpace(RE::TESObjectREFR* ref)
        {
            static REL::Relocation<bool (*)(RE::TESObjectREFR*, bool)> fn{ REL::ID(kInSpaceId) };
            return fn(ref, false);
        }

        float Distance(const RE::NiPoint3& a, const RE::NiPoint3& b)
        {
            return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z);
        }

        // Saves are blocked here, and were not when we started: only then is unblocking ours to do.
        bool g_weBlockedSaves = false;
        bool g_hold = false;  // '@checkpointhold'
        std::uint32_t g_marker = 0;
        std::shared_ptr<std::atomic<std::uint32_t>> g_markerPending;
        RE::NiPoint3 g_checkpoint{};
        RE::TESObjectCELL* g_checkpointCell = nullptr;
        bool g_haveCheckpoint = false;
        float g_lastZ = 0.0f;
        Clock::time_point g_nextCheckpoint{};
        Clock::time_point g_graceUntil{};
        Clock::time_point g_nextTopUp{};
        Clock::time_point g_verifyAt{};
        RE::NiPoint3 g_verifyTarget{};
        const char* g_verifyKind = "";
        std::uint32_t g_returns = 0;
        bool g_warnedDead = false;

        bool IsMarker(std::uint32_t id)
        {
            auto* ref = avatars::AsRef(RE::TESForm::LookupByID(id));
            if (!ref) return false;
            const auto base = ref->GetBaseObject();
            return base && base->GetFormID() == kMarkerBaseId;
        }

        void UpdateSaveBlock(const Context& context)
        {
            const bool want = context.inSession && !context.ownsWorld;
            if (want) {
                // Reasserted while it lasts, because the game's own scripts also set this flag.
                if (!SavingBlockedBit()) {
                    WriteSavingBlockedBit(true);
                    if (!g_weBlockedSaves) log::Info("[respawn] saving is off on this copy: it does not own the session's world");
                    g_weBlockedSaves = true;
                }
            } else if (g_weBlockedSaves) {
                WriteSavingBlockedBit(false);
                g_weBlockedSaves = false;
                log::Info("[respawn] saving is on again");
            }
        }

        // A safe spot: not fighting, not hurt, standing, on the ground or a ship deck, not in space.
        void UpdateCheckpoint(RE::PlayerCharacter* player, Clock::time_point now)
        {
            if (g_markerPending) {
                const auto result = g_markerPending->load();
                if (result == 0) return;
                g_markerPending.reset();
                if (avatars::IsSpawnError(result)) {
                    log::Warn("[respawn] checkpoint marker failed: {}", avatars::DescribeSpawnError(result));
                } else {
                    g_marker = result;
                    g_checkpoint = player->GetPosition();
                    g_checkpointCell = player->parentCell;
                    g_haveCheckpoint = true;
                    log::Info("[respawn] checkpoint {:08X} at {:.1f} {:.1f} {:.1f}", g_marker, g_checkpoint.x, g_checkpoint.y, g_checkpoint.z);
                }
                return;
            }
            if (g_hold || now < g_nextCheckpoint || now < g_graceUntil) return;
            g_nextCheckpoint = now + kCheckpointEvery;
            const auto at = player->GetPosition();
            const bool level = std::fabs(at.z - g_lastZ) < kFallingPerCheck;
            g_lastZ = at.z;
            const auto id = avatars::PlayerRefId();
            if (!level || !player->parentCell || npcsync::ActorHealthPercent(id) < kSafeHealthPercent || npcsync::ActorInCombat(id) || InSpace(player)) return;
            if (g_marker == 0) {
                auto slot = std::make_shared<std::atomic<std::uint32_t>>(0);
                std::string error;
                if (avatars::RequestMarker(slot, error)) g_markerPending = std::move(slot);
                else log::Warn("[respawn] checkpoint marker not placed: {}", error);
                return;
            }
            if (g_haveCheckpoint && player->parentCell == g_checkpointCell && Distance(at, g_checkpoint) < kCheckpointMoved) return;
            if (avatars::MoveRefTo(g_marker, id)) {
                g_checkpoint = at;
                g_checkpointCell = player->parentCell;
                g_haveCheckpoint = true;
            }
        }

        // The last safe spot if it is still loaded, otherwise beside another player, otherwise nothing:
        // the player then stays where they are, at full health.
        std::uint32_t ChooseReturn(const Context& context, const char*& kind, RE::NiPoint3& at)
        {
            if (g_marker != 0 && g_haveCheckpoint) {
                auto* marker = avatars::AsRef(RE::TESForm::LookupByID(g_marker));
                if (marker && marker->parentCell && marker->parentCell->IsAttached()) {
                    kind = "checkpoint";
                    at = g_checkpoint;
                    return g_marker;
                }
            }
            for (const auto standIn : context.standIns) {
                if (auto* ref = avatars::AsRef(RE::TESForm::LookupByID(standIn)); ref && ref->parentCell) {
                    kind = "another player";
                    at = ref->GetPosition();
                    return standIn;
                }
            }
            kind = "nowhere safe";
            return 0;
        }

        void Return(const Context& context, RE::PlayerCharacter* player, std::uint32_t caught, Clock::time_point now)
        {
            const auto id = avatars::PlayerRefId();
            const auto from = player->GetPosition();
            npcsync::RestoreActorHealth(id);
            g_graceUntil = now + kGrace;
            g_nextTopUp = now + kGraceTopUp;
            ++g_returns;
            const char* kind = "";
            RE::NiPoint3 to{};
            const auto target = ChooseReturn(context, kind, to);
            bool sent = false;
            if (target != 0) sent = avatars::MoveRefTo(id, target);
            log::Info("[respawn] the player would have died (blow caught {}x) at {:.1f} {:.1f} {:.1f}; return #{} to {} {:08X} at {:.1f} {:.1f} {:.1f}: {}", caught, from.x, from.y, from.z,
                      g_returns, kind, target, to.x, to.y, to.z, target == 0 ? "stayed put" : sent ? "sent" : "refused");
            avatars::Notify(target == 0 ? "You were saved from death." : "You were brought back from death.");
            g_verifyAt = now + kVerifyAfter;
            g_verifyTarget = target == 0 ? from : to;
            g_verifyKind = kind;
        }
    }

    bool Install(std::string& detail)
    {
        const auto init = MH_Initialize();
        const auto target = reinterpret_cast<void*>(REL::ID(kKillId).address());
        const bool ok = (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED) &&
                        MH_CreateHook(target, reinterpret_cast<void*>(&KillHook), reinterpret_cast<void**>(&g_kill)) == MH_OK && MH_EnableHook(target) == MH_OK;
        if (ok) {
            REL::Relocation<std::uintptr_t> table{ REL::ID(kPlayerVtableId) };
            g_death = reinterpret_cast<Death_t>(table.write_vfunc(kDeathSlot, &DeathHook));
            g_setLife = reinterpret_cast<SetLife_t>(table.write_vfunc(kLifeStateSlot, &SetLifeHook));
        }
        detail = ok ? "kill dispatcher and the player's death slots hooked" : "kill dispatcher NOT hooked: a death in a session will reload a save";
        return ok;
    }

    void Tick(const Context& context)
    {
        UpdateSaveBlock(context);
        const bool live = context.inSession && context.inWorld;
        // The whole session, not only while in the world: a blow during a stall must not reload a save.
        g_active.store(context.inSession, std::memory_order_relaxed);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!live || !player) return;
        // A load discards the marker, and its number can come back as something else.
        if (g_marker != 0 && !IsMarker(g_marker)) {
            log::Info("[respawn] checkpoint {:08X} is gone; placing a new one", g_marker);
            g_marker = 0;
            g_haveCheckpoint = false;
            g_checkpointCell = nullptr;
        }
        const auto now = Clock::now();

        // A way into death that does not pass the dispatcher would still reload the save. Say so.
        if (const auto life = LifeStateBits(); (life == kDeadState1 || life == kDeadState2) && !g_warnedDead) {
            g_warnedDead = true;
            log::Warn("[respawn] the player is in a death state ({:#x}) though the kill was stopped: a reload will follow", life);
        }

        if (const auto caught = g_caught.exchange(0, std::memory_order_relaxed); caught != 0) {
            if (now >= g_graceUntil) Return(context, player, caught, now);
            else npcsync::RestoreActorHealth(avatars::PlayerRefId());
        }
        if (now < g_graceUntil && now >= g_nextTopUp) {
            g_nextTopUp = now + kGraceTopUp;
            npcsync::RestoreActorHealth(avatars::PlayerRefId());
        }
        if (g_verifyAt != Clock::time_point{} && now >= g_verifyAt) {
            g_verifyAt = {};
            const auto at = player->GetPosition();
            log::Info("[respawn] verify: {:.1f} from {} target, health {:.1f}%, life state {:#x}, position {:.1f} {:.1f} {:.1f}", Distance(at, g_verifyTarget), g_verifyKind,
                      npcsync::ActorHealthPercent(avatars::PlayerRefId()), LifeStateBits(), at.x, at.y, at.z);
        }
        UpdateCheckpoint(player, now);
    }

    void Leave()
    {
        g_active.store(false, std::memory_order_relaxed);
        g_caught.store(0, std::memory_order_relaxed);
        if (g_weBlockedSaves) {
            WriteSavingBlockedBit(false);
            g_weBlockedSaves = false;
            log::Info("[respawn] session over: saving is on again");
        }
        if (g_marker != 0) avatars::Despawn(g_marker);
        g_marker = 0;
        g_markerPending.reset();
        g_haveCheckpoint = false;
        g_checkpointCell = nullptr;
        g_hold = false;
        g_warnedDead = false;
    }

    bool RunStep(const std::string& step)
    {
        const auto id = avatars::PlayerRefId();
        if (step == "@killself") {
            log::Info("[respawn] {}: Actor.Kill on the player: {}", step, avatars::KillActor(id) ? "sent" : "refused");
            return true;
        }
        if (step == "@killselfhit") {
            log::Info("[respawn] {}: lethal damage on the player: {}", step, npcsync::DamageActorPercent(id, 300.0f) ? "sent" : "refused");
            return true;
        }
        if (step == "@cansave") {
            const auto allowed = EngineAllowsSaving();
            log::Info("[respawn] @cansave: engine says {}, saving-disabled flag {}", !allowed ? "no manager" : *allowed ? "allowed" : "refused", SavingBlockedBit());
            return true;
        }
        if (step.starts_with("@savetest ")) {
            dev::RunConsole("save " + step.substr(10));
            return true;
        }
        if (step == "@checkpoint") {
            log::Info("[respawn] @checkpoint: {:08X} at {:.1f} {:.1f} {:.1f} ({})", g_marker, g_checkpoint.x, g_checkpoint.y, g_checkpoint.z, g_haveCheckpoint ? "set" : "none");
            return true;
        }
        if (step.starts_with("@checkpointhold ")) {
            g_hold = step.ends_with('1');
            log::Info("[respawn] {}", step);
            return true;
        }
        return false;
    }
}
