#pragma once

#include "cosync/shared/Types.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

// Remote-player stand-ins ("avatars") via the game's Papyrus VM.
//
// CommonLibSF exposes no native reference-creation function, so placement
// goes through Papyrus. Signatures were read from the game's compiled scripts
// (Starfield - Misc.ba2, scripts/objectreference.pex and actor.pex, 1.16.244):
//   ObjectReference PlaceAtMe(Form, Int, Bool, Bool, Bool, Float[], alias, Bool)  (rejected: see Avatars.cpp)
//   actor PlaceActorAtMe(actorbase, Int, location, Bool, Bool, Bool, Float[], Bool)  (used)
//   None SetPosition(Float, Float, Float)      None SetAngle(Float, Float, Float)  (degrees)
//   None EnableAI(Bool abEnable, Bool abPauseVoice)
//   None Disable(Bool abFadeOut)               None Delete()
// Calls are queued on the VM and complete asynchronously. All functions here
// must be called on the MAIN THREAD; the spawn result arrives on a VM thread
// and is published through the atomic.
namespace cosync::plugin::avatars
{
    // Spawn results that are not a FormID (diagnostic detail for logs).
    inline constexpr std::uint32_t kSpawnCanceled = 0xFFFFFFF0;      // VM cancelled the call
    inline constexpr std::uint32_t kSpawnReturnedNone = 0xFFFFFFF1;  // placement returned None
    inline constexpr std::uint32_t kSpawnWrongType = 0xFFFFFFF2;     // result was not an object
    inline constexpr std::uint32_t kSpawnUnpackFailed = 0xFFFFFFF3;  // object, but no reference behind it
    [[nodiscard]] constexpr bool IsSpawnError(std::uint32_t v) noexcept { return v >= kSpawnCanceled; }
    [[nodiscard]] const char* DescribeSpawnError(std::uint32_t v) noexcept;

    // Radius for picking a template actor near the player (engine units).
    inline constexpr float kTemplateSearchRadius = 4096.0f;

    [[nodiscard]] std::uint32_t PlayerRefId();

    // Asks the VM for a random actor near the local player (Game.FindRandomActor).
    // *result receives that actor's FormID, kSpawnReturnedNone if there is none,
    // or another kSpawn* error. It may be the player itself; callers retry.
    bool RequestTemplateActor(std::shared_ptr<std::atomic<std::uint32_t>> result, std::string& error);

    // Asks the VM for an actor's base as scripts see it (ObjectReference.GetBaseObject).
    // In memory, loaded NPCs carry bases generated at runtime (FFxxxxxx), and
    // PlaceActorAtMe returns None for those (2026-10-27). *result receives the
    // base's FormID if it is an NPC_, else a kSpawn* error.
    bool RequestBaseObject(std::uint32_t actorId, std::shared_ptr<std::atomic<std::uint32_t>> result,
                           std::string& error);

    // True if `baseId` is an NPC_ defined in a data file (not generated at runtime).
    [[nodiscard]] bool IsPlaceableBase(std::uint32_t baseId);

    // Places an actor of base NPC `baseNpcId` (0, or not placeable, falls back to
    // the player's base) at the local player. When the VM finishes, *result is
    // set to the new reference's FormID or a kSpawn* error code. Returns false
    // and sets `error` if it could not be dispatched; `detail` describes the base.
    bool RequestSpawn(std::uint32_t baseNpcId, std::shared_ptr<std::atomic<std::uint32_t>> result,
                      std::string& error, std::string& detail);

    // True if the Papyrus VM is running normally. During a save load the VM is
    // frozen (Papyrus.0.log: "VM is freezing" ... "VM is thawing") and calls
    // dispatched then are dropped without a reply (observed 2026-10-27).
    // Reads BSScript::Internal::VirtualMachine::freezeState and ::overstressed.
    [[nodiscard]] bool VmReady();

    // Stops the avatar's AI so the game does not steer it. False if the ref is gone.
    bool Freeze(std::uint32_t refId);

    // Moves the avatar (Papyrus SetPosition/SetAngle). Rotation in radians.
    // False if the reference no longer exists.
    bool Move(std::uint32_t refId, const Vec3& position, const Vec3& rotationRadians);

    // ---- Engine-driven movement (path following) ------------------------------
    // Teleporting with SetPosition never plays locomotion (the graph variables are
    // overwritten from the actor's real movement; Starfield's player graph has no
    // move-start/stop events, 2026-11-02). Instead the proxy follows an invisible
    // marker with the engine's own pathing, which animates walk/run like any NPC.
    //
    // Places the vanilla XMarker (STAT 0000003B) at the local player. *result gets the
    // marker's FormID or a kSpawn* error. Papyrus: ObjectReference PlaceAtMe(Form, Int,
    // Bool, Bool, Bool, Float[], alias, Bool).
    bool RequestMarker(std::shared_ptr<std::atomic<std::uint32_t>> result, std::string& error);
    // SetPosition/SetAngle on any reference (marker or actor). Rotation in radians.
    bool MoveRef(std::uint32_t refId, const Vec3& position, const Vec3& rotationRadians);
    // Starfield's signature (scripts/actor.pex in "Starfield - Misc.ba2", read with
    // tools/re/ba2pex.py): Int PathToReference(ObjectReference aTarget, Float afNormalizedSpeed,
    // Float afNormalizedRotationSpeed, Float afTargetRadius, Bool abHardRadius), latent.
    // *done is set to (returned Int + 1) when it returns (so 0 means in flight), or
    // 0xFFFFFFF0 if the call was cancelled.
    bool RequestPathTo(std::uint32_t actorId, std::uint32_t targetId, float normalizedSpeed, float targetRadius,
                       std::shared_ptr<std::atomic<std::uint32_t>> done);

    // ---- Weapon (signatures from the game's actor.pex / objectreference.pex) -------
    //   weapon GetEquippedWeapon(Int aiEquipIndex)   Bool IsWeaponDrawn()
    //   None AddItem(Form, Int aiCount, Bool abSilent)
    //   None EquipItem(Form, Bool abPreventRemoval, Bool abSilent)   None UnequipItem(Form, Bool, Bool)
    //   None DrawWeapon()   None SheatheWeapon()
    struct WeaponQuery
    {
        std::atomic<std::uint32_t> formId{ 0 };  // 0 = none
        std::atomic<int> drawn{ -1 };            // -1 unknown, 0, 1
        std::atomic<int> pending{ 2 };           // replies outstanding
    };
    bool RequestWeaponState(std::uint32_t actorId, std::shared_ptr<WeaponQuery> q);
    // AddItem(1, silent) then, when that has run, EquipItem(preventRemoval, silent).
    bool EquipWeapon(std::uint32_t actorId, std::uint32_t weaponFormId);
    bool UnequipWeapon(std::uint32_t actorId, std::uint32_t weaponFormId);
    // DrawWeapon/SheatheWeapon plus SetAlert(drawn): an alerted NPC keeps its weapon out instead of
    // relaxing it (2026-11-03: drawn by DrawWeapon alone, it was put away again within ~1 s).
    bool SetWeaponDrawn(std::uint32_t actorId, bool drawn);

    // ---- Combat (actor.pex: Actor GetCombatTarget(); Bool StartCombat(ObjectReference, Bool)) ----
    // *result = target FormID, 1 = no target, 0 = in flight.
    bool RequestCombatTarget(std::uint32_t actorId, std::shared_ptr<std::atomic<std::uint32_t>> result);
    bool StartCombat(std::uint32_t actorId, std::uint32_t targetRefId);
    bool StopCombat(std::uint32_t actorId);  // Papyrus Actor.StopCombat()
    bool KillSilent(std::uint32_t actorId);  // Papyrus Actor.KillSilent(None)

    // ---- World (RE 12.11) ----
    bool SetOpen(std::uint32_t refId, bool open);      // ObjectReference.SetOpen(bool)
    bool DisableNoWait(std::uint32_t refId);           // ObjectReference.DisableNoWait(bool fade)
    bool SetQuestStage(std::uint32_t questId, int stage);  // Quest.SetCurrentStageID(int)
    bool ActivateAsPlayer(std::uint32_t refId);        // ObjectReference.Activate(player, false)
    bool ContainerAdd(std::uint32_t refId, std::uint32_t itemId, int count);     // AddItem(item, count, silent)
    bool ContainerRemove(std::uint32_t refId, std::uint32_t itemId, int count);  // RemoveItem(item, count, silent, None)
    bool ContainerTakeAll(std::uint32_t refId);        // RemoveAllItems(player, false, false)
    // Armor, clothing or a weapon: added if missing, then equipped. Unequip leaves it in the inventory.
    bool EquipForm(std::uint32_t actorId, std::uint32_t itemFormId);
    bool UnequipForm(std::uint32_t actorId, std::uint32_t itemFormId);
    // Papyrus ObjectReference.DamageValue(Game.GetHealthAV(), amount): the engine's whole damage
    // path, with its bleedout and death handling (RE 12.1).
    bool DamageHealth(std::uint32_t actorId, float amount);

    // ---- Local player status (game.pex: actorvalue GetHealthAV(); objectreference.pex:
    // Float GetValuePercentage(actorvalue), planet GetCurrentPlanet(), Bool IsInSpace();
    // actor.pex: Int GetLevel()). Planet name from its TESFullName.
    struct StatusQuery
    {
        std::atomic<int> pending{ 4 };
        std::atomic<int> healthPercent{ -1 };
        std::atomic<int> level{ -1 };
        std::atomic<int> inSpace{ -1 };
        std::mutex lock;
        std::string planet;
    };
    bool RequestStatus(std::uint32_t actorId, std::shared_ptr<StatusQuery> q);

    // Papyrus ObjectReference.SetScale(Float) (1 param, registered in the exe). Debug aid.
    bool SetScale(std::uint32_t refId, float scale);

    // Disables and deletes the avatar. Safe to call with a stale id.
    void Despawn(std::uint32_t refId);

    // ---- Animation graph variables ------------------------------------------
    // Papyrus ObjectReference natives, all registered in Starfield.exe 1.16.244:
    //   Float GetAnimationVariableFloat(String)  native 0x2070580
    //   Int   GetAnimationVariableInt(String)    native 0x2070460
    //   Bool  GetAnimationVariableBool(String)   (wrapper 0x209FCB0)
    //   None  SetAnimationVariableFloat(String, Float) native 0x207F9C0
    //   None  SetAnimationVariableBool(String, Bool)   native 0x207F8B0
    // Variable names are graph-defined; candidates come from strings in the exe
    // (Speed, SpeedSampled, Direction, IsMoving, ...), verified at runtime by reading
    // them on the local player.
    enum class AnimVarKind : char { kFloat = 'f', kInt = 'i', kBool = 'b' };
    struct AnimVar
    {
        const char* name;
        AnimVarKind kind;
    };

    // Asynchronous read of several variables. Each slot holds the float bits of the
    // value (ints and bools converted) once its call returns; `remaining` counts down.
    struct AnimReadout
    {
        explicit AnimReadout(std::size_t n) : bits(n) {}
        std::vector<std::atomic<std::uint32_t>> bits;
        std::atomic<int> remaining{ 0 };
        std::atomic<int> failed{ 0 };
    };
    // Returns nullptr if the ref is gone or no call could be dispatched.
    [[nodiscard]] std::shared_ptr<AnimReadout> RequestAnimVars(std::uint32_t refId, std::span<const AnimVar> vars);
    [[nodiscard]] std::string FormatReadout(const AnimReadout& r, std::span<const AnimVar> vars);

    bool SetAnimFloat(std::uint32_t refId, const char* name, float value);
    bool SetAnimBool(std::uint32_t refId, const char* name, bool value);

    // Engine-side state of a spawned avatar, read synchronously on the MAIN THREAD.
    // Every read is backed by disassembly of Starfield.exe 1.16.244 (docs/REVERSE_ENGINEERING.md):
    //   form type   TESForm+0x2E == 0x4B (ACHR)       PlaceActorAtMe native 0x207A220 compares it
    //   disabled    TESForm+0x20 bit 11              ObjectReference.IsDisabled native 0x208BAB0
    //   deleted     TESForm+0x20 bit 5               CommonLibSF kDeleted (not yet traced in the exe)
    //   parent cell TESObjectREFR+0xB0               PlaceActorAtMe passes it to vfunc 0x10C (SetParentCell)
    //   3D          vtbl[0xAC](NiPointer<NiAVObject>&) ObjectReference.Is3DLoaded native 0x2075F00
    struct ProxyState
    {
        bool exists = false;        // LookupByID found a form
        bool isActor = false;       // form type ACHR
        bool disabled = false;
        bool deleted = false;
        bool has3D = false;         // Get3D returned a scene-graph node
        bool sameCellAsPlayer = false;
        std::uint32_t parentCellId = 0;
        std::uint32_t baseId = 0;
        Vec3 position;
        float distanceToPlayer = -1.0f;
    };
    [[nodiscard]] ProxyState Probe(std::uint32_t refId);
    // Game.GetPlayerHomeSpaceShip. *result receives the ship's FormID, or 1 when there is none.
    bool RequestHomeShip(std::shared_ptr<std::atomic<std::uint32_t>> result);
    // Game.TrySetPlayerHomeSpaceShip. *result receives 2 on success, 1 on refusal.
    bool RequestSetHomeShip(std::uint32_t shipId, std::shared_ptr<std::atomic<std::uint32_t>> result);
    // ObjectReference.MoveTo(target, 0, 0, 0, true, false): 0x2077B90 takes a target, three offsets, two flags.
    bool MoveRefTo(std::uint32_t refId, std::uint32_t targetId);
    // ObjectReference.PlaceShipAtMe (0x207A8B0) on `anchorId`: the ship base, then a level mod, four
    // flags and three optional forms (None here) and a last flag. *result receives the new ship's
    // FormID, or a kSpawn* error.
    bool RequestPlaceShip(std::uint32_t anchorId, std::uint32_t shipBaseId, std::shared_ptr<std::atomic<std::uint32_t>> result);
    // ObjectReference.SetPosition / SetAngle (degrees) on any reference.
    bool PlaceRef(std::uint32_t refId, const Vec3& position, const Vec3& angleRadians);
    // Actor.StartCombat against the local player, and Actor.Kill with no killer.
    bool StartCombatWithPlayer(std::uint32_t actorId);
    bool KillActor(std::uint32_t actorId);
    // Debug.Notification: a line at the top left of the HUD.
    bool Notify(const std::string& text);
    bool SetLocked(std::uint32_t refId, bool locked);
    bool DamageObject(std::uint32_t refId, float amount);
    bool SetDestroyed(std::uint32_t refId, bool destroyed);
    [[nodiscard]] std::string Describe(const ProxyState& s);
}
