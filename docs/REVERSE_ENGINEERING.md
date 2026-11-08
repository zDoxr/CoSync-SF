# CoSync-SF — Reverse-Engineering Evidence

Verified engine facts for Starfield, with the evidence behind each one. Anything not proven here is marked **UNKNOWN** or **INFERRED**. Add new findings in the same format.

## Target binary

| Field | Value |
|---|---|
| Game | Starfield (Steam) **1.16.244.0** |
| File | `Starfield.exe`, 102,476,200 bytes |
| SHA-256 | `7e9adb1414a8e1b325e5e1f097b9b17b78deb7eebeda37a333351a43a60f9d28` |
| Image base | `0x140000000` (all addresses below are **RVAs**; VA = base + RVA) |
| `.text` | RVA `0x1000`, size `0x3A3DF6C` (not packed/encrypted: disassembles cleanly) |
| `.pdata` | 236,427 RUNTIME_FUNCTION entries (exact function bounds) |
| Address Library | `versionlib-1-16-244-0.bin`, format v5 (`uint32 rva[id]` at file offset `0x60`), game version 1.16.244.0 |
| SFSE | 0.2.21 (`sfse_1_16_244.dll`) |

## Method and tools

- For the first findings, no disassembler was installed. IDA Pro 9.3 (with the x64 decompiler) has since been added; its batch scripts live in `tools/re/ida/`.
- Analysis instead uses `tools/re/sfre.py`, a small, reproducible static-analysis tool (Python + pefile + capstone + numpy). It:
  - maps the PE image and takes function bounds from `.pdata` (following chained unwind info);
  - resolves Address Library IDs exactly as CommonLibSF does (`commonlib-shared/src/REL/IDDB.cpp`, `load_v5`);
  - finds code references (rip-relative `lea`/`mov`, `call`/`jmp rel32`) by scanning all of `.text`, decoding every candidate;
  - resolves MSVC RTTI: type descriptor → complete object locator (COL) → vtables.
- `tools/re/natives.py` lists Papyrus native registrations: the class string, the argument count where the registration template stores it, and the native function pointer.
- Every claim below can be reproduced with commands such as `python tools/re/sfre.py func 0x207a220`. (Run without `-I`: capstone lives in the user site-packages.)
- Runtime validation comes from the CoSync plugin's own logs (`CoSyncSF-p<n>.log`) in two-instance tests.

Confidence levels:
- **VERIFIED**: seen in disassembly and cross-checked (RTTI, Address Library ID, or runtime).
- **VERIFIED-RT**: also confirmed at runtime in game.
- **INFERRED**: consistent with disassembly but not independently proven.
- **UNKNOWN**.

---

## 1. Papyrus native registration (how script functions map to engine code)

| Item | Evidence | Confidence |
|---|---|---|
| ObjectReference natives are registered in one large function at RVA `0x2085050`. | Contains `lea r8,"ObjectReference"` + `lea rdx,"<name>"` + `lea r9,<native>` before each per-native constructor call. | VERIFIED |
| A per-native wrapper object stores the native function pointer at `+0x50`. | `0x2097650` (PlaceActorAtMe wrapper ctor): `mov [rdi+0x50], rax` with `rax = 0x207A220`. The `Is3DLoaded` wrapper ctor `0x209B180` stores `r9` at `+0x50`. | VERIFIED |
| The argument count is passed as the 5th argument (`[rsp+0x20]`) of the base wrapper ctor `0x28249C0`. | `0x2097677: mov dword [rsp+0x20], 8` for PlaceActorAtMe (8 params, which matches `actor.pex`). | VERIFIED |
| Native calling convention: `rcx` = VM, `edx` = stack id, `r8` = self, `r9` = 1st script arg, then the stack. | PlaceActorAtMe: `r9` is null-checked as "Cannot place a None actor"; the later args are read from `[rbp+0xA0..0xD0]`, which are the 5th to 11th native arguments. | VERIFIED |
| The form of `r8` (self) depends on the binding template. | For `IsDisabled` (`0x208BAB0`), `r8` **is** the ref (`mov eax,[r8+0x20]`). For `Is3DLoaded` (`0x2075F00`), `r8` **points to** the ref (`mov rcx,[r8]`). | VERIFIED (the template families are not characterized) |

Papyrus natives used by CoSync (registration located in the exe):

| Script function | Params | Native RVA | Notes |
|---|---|---|---|
| `ObjectReference.PlaceActorAtMe` | 8 | `0x207A220` | §2 |
| `ObjectReference.Is3DLoaded` | 0 | `0x2075F00` | §4 |
| `ObjectReference.IsDisabled` | 0 | `0x208BAB0` | §4 |
| `ObjectReference.GetBaseObject` | 0 | `0x20706A0` | |
| `ObjectReference.SetPosition` / `SetAngle` | 3 | wrapper `0x20901B0` | |
| `ObjectReference.SetScale` | 1 | wrapper `0x20A1E30` | |
| `ObjectReference.GetDistance` | 1 | wrapper `0x209E250` | |
| `Actor.EnableAI` | 2 | thunk `0x1FD1020` → `0x1959170` | §5 |
| `Game.FindRandomActor` | 4 | wrapper `0x20197F0` | |

---

## 2. Native actor creation: `ObjectReference.PlaceActorAtMe` (`0x207A220`)

This is the engine's own script-facing pathway for creating an NPC at runtime, and **CoSync's proxy spawn goes through it**. Annotated disassembly (abridged):

```
207A25F  test r9,r9 ; jne ...              ; akActorToPlace == None?
207A279  lea  rax,"Cannot place a None actor"            -> VM error, return None
207A2C9  mov  eax,[r9+0x28]                ; base->formID
207A2CD  add  eax,0x1000000
207A2D2  cmp  eax,0xFFFFFE ; ja ok          ; formID in FF000000..FFFFFFFE ?
207A2E7  lea  rdx," cannot be placed with PlaceActorAtMe, as it's a runtime-created form"
207A2F1  call 0x1FA4550                    ; -> VM error, return None
207A303  call 0x20789E0                    ; self ref (into [rsp+48])
207A333  lea  rcx,MemoryManager(0x5E603C0) ; edx=0x580, r8d=0x10
207A33A  call 0x22C7AD0                    ; MemoryManager::Allocate (ID 123792)
207A359  call 0x18D77E0                    ; Actor::Actor(mem, &tmp, 1, 0)
207A36E  lock xadd [r14+8],...              ; refcount/handle bookkeeping on the new actor
207A39B  (akEncLoc != None) -> call 0x51B080(extraList@+0xC8, actor, loc); vfunc[0x17](0x20000000)
207A3CA  call 0x51A630(extraList, aiLevelMod) ; vfunc[0x17](0x40000000)
207A3EC  call [vtbl+0x7B8]  (actor, base, 1)  ; vfunc 0xF7 (INFERRED: set base object)
207A40B  call [vtbl+0x860]  (actor, self->parentCell@+0xB0)   ; vfunc 0x10C SetParentCell
207A4A3  call 0x2079960(self, akOffsetValues, stackId, vm, &pos, &rot, abSnapOffsetToNavmesh) ; INFERRED: placement transform
207A4AD  call 0xB4EB40                     ; TESObjectREFR::GetParentWorldSpace (ID 63417)
207A4B9..55B  build a new-reference descriptor on the stack (see §3)
207A56A  mov  rcx,[0x5FD9B28]              ; TESDataHandler singleton (ID 937572)
207A571  call 0x5A0520                     ; TESDataHandler::CreateReference... (ID 46570)
207A5E6  and  dword [rbx+0x20],0xFFDFFFFF   ; clear form flag 0x200000 on the new actor
207A5F4  (abDeleteWhenAble) -> call 0xB3F5D0(actor, 1)
207A66B  mov  rax,rsi                      ; return the new Actor
```

Results:
- **VERIFIED.** Runtime-created bases (`FFxxxxxx`) are rejected by design. This is the root cause of the 2026-10-27 "PlaceActorAtMe returned None" failures, when the template base was `FF013A02`.
- **VERIFIED.** The spawn sequence is: allocate `0x580` bytes, run `Actor::Actor`, set the base and level modifier, `SetParentCell(self->parentCell)`, compute the transform, then `TESDataHandler::CreateReference(...)`. The engine registers the reference itself; no step is left to the caller.
- **VERIFIED-RT.** The returned actor is a real `ACHR` in the player's cell, has no 3D for the first frames, and has its 3D loaded about 1 s later (log, 2026-11-02: `3D=NONE` at spawn, `3D=loaded` at +1 s, on both copies).
- Allocation size `0x580` vs CommonLibSF `sizeof(Actor) == 0x578`: the size is rounded to the `0x10` alignment passed to `Allocate`. INFERRED.

## 3. `TESDataHandler::CreateReference` (`0x5A0520`, Address Library ID 46570)

Signature (INFERRED from both call sites): `void* __fastcall (TESDataHandler* this, void* out, NewRefData* data)`; `rcx` = `[0x5FD9B28]`. The function is large (`0x5A0520..0x5A25DF`).

| Finding | Evidence | Confidence |
|---|---|---|
| If the descriptor carries no ready-made ref and the base is an NPC, it constructs the Actor itself: Allocate `0x580` → `Actor::Actor` (`0x18D77E0`). If `data+0x60` is non-null, it uses a second Actor ctor overload `0x18D7C90`. | `0x5A12C2..0x5A135D` | VERIFIED |
| `0x18D7C90` is an **Actor** constructor, not a different class. | It writes the same `.?AVActor@@` vtables (`0x4CB9248` at +0, …). | VERIFIED |
| It is the same function that PlaceActorAtMe calls with a pre-built actor. | §2, `0x207A571` | VERIFIED |

Descriptor layout as filled by PlaceActorAtMe (`[rbp-0x60]` = descriptor `+0x00`):

| Offset | Value written | Meaning | Confidence |
|---|---|---|---|
| +0x00 | `0x4B0F6C8` | a constant pointer (vtable-like) | INFERRED |
| +0x08 | 0 | | |
| +0x10 | 12 bytes from `[rsp+0x60]` | rotation (output of `0x2079960`) | INFERRED |
| +0x1C | 12 bytes from `[rsp+0x70]` | position | INFERRED |
| +0x28 | `1.0f` (`0x4F044E8`) | scale | INFERRED |
| +0x30 | `actor+0x98` (`data.objectReference`) | base object | VERIFIED (CommonLibSF `OBJ_REFR` at +0x80, `objectReference` at +0x18) |
| +0x38 | parent cell (null if `cell+0x48 & 1`) | cell | VERIFIED (CreateReference reads `[data+0x38]` as a cell, compares `cell+0x110` with `[data+0x40]`) |
| +0x40 | `GetParentWorldSpace(self)` | worldspace | VERIFIED |
| +0x48 | the new actor | pre-constructed ref | VERIFIED |
| +0x60 | 0 | when non-null, CreateReference uses ctor `0x18D7C90` | VERIFIED |
| +0x8B | abForcePersist | | VERIFIED (byte copied from that argument) |
| +0x8C..0x8D | `0x0101` | UNKNOWN flags | |
| +0x8E | abInitiallyDisabled | | VERIFIED |
| others | zero | UNKNOWN | |

**UNKNOWN:** the full size of the descriptor, the meaning of `+0x8C/+0x8D/+0x90/+0x91/+0x96`, and the semantics of the `out` slot. These are required before CoSync can call `0x5A0520` directly instead of going through Papyrus. That change would make spawning synchronous; it is **not** required for the PoC.

## 4. Object and reference layout reads (used by `avatars::Probe`)

| Field | Offset | Proof in the exe | Confidence |
|---|---|---|---|
| `TESForm::formFlags` | +0x20 | `IsDisabled` native `0x208BAB0`: `mov eax,[r8+0x20]; shr eax,0xB; and al,1` | VERIFIED |
| `kDisabled` flag | bit 11 | same | VERIFIED |
| `kDeleted` flag | bit 5 | CommonLibSF only | INFERRED |
| `TESForm::formID` | +0x28 | PlaceActorAtMe `mov eax,[r9+0x28]` used in the runtime-form check | VERIFIED |
| `TESForm::formType` | +0x2E (u8), ACHR = `0x4B` | `cmp byte [rax+0x2E],0x4B` in PlaceActorAtMe and CreateReference | VERIFIED |
| `TESObjectREFR::data.objectReference` (base) | +0x98 | PlaceActorAtMe passes `[actor+0x98]` as the descriptor's base | VERIFIED |
| `TESObjectREFR::parentCell` | +0xB0 | PlaceActorAtMe passes `[self+0xB0]` to vfunc `0x10C` (SetParentCell) and to the descriptor's cell | VERIFIED-RT (proxy cell == player cell in logs) |
| `TESObjectREFR::data.location` | +0x8C | CommonLibSF; matches the in-game reported positions | VERIFIED-RT |
| **Get3D** | vfunc **0xAC** (`vtbl+0x560`), `NiPointer<NiAVObject>* (this, NiPointer<NiAVObject>* out)` | `Is3DLoaded` native `0x2075F00`: `call [rax+0x560]` with `rdx=&out`, then `cmp qword [rax],0`, then releases `out` | VERIFIED-RT |
| NiRefObject refcount / release | +0x08, `DeleteThis` = vfunc 1 | the same function: `lock xadd [rcx+8],-1; if 1 → call [vtbl+8]`, which matches CommonLibSF `NiRefObject` | VERIFIED |
| `SetParentCell` | vfunc 0x10C (`vtbl+0x860`) | matches CommonLibSF's declaration | VERIFIED |
| `TESObjectREFR::loadedData` | +0xB8 | CommonLibSF only. Not read by CoSync; use Get3D instead. | UNKNOWN |

## 5. `Actor::EnableAI(bool enable, bool pauseVoice)` (`0x1959170`)

Papyrus `Actor.EnableAI` → thunk `0x1FD1020` (`rcx=self, dl=abEnable, r8b=abPauseVoice`) → `0x1959170`.

- Sets or clears bit 1 of `Actor+0x208` (set = AI enabled). VERIFIED.
- When disabling with `pauseVoice`, it sets bit 20 of `+0x208` and calls vfunc `0x82` (`vtbl+0x410`). When re-enabling, it clears that bit and conditionally calls vfunc `0x83`. VERIFIED. The voice semantics are INFERRED from the parameter name.
- It then dereferences `Actor+0x228` → `+0x8` → `+0x458` (INFERRED: process / animation-side object) and calls `0x23B20F0(obj, flag)`, then vfunc `0x278/8` on that object. If 3D is loaded (Get3D, `vtbl+0x560`), it also updates further objects under `+0x228 → +0x10`. VERIFIED instruction-level; the purpose is UNKNOWN.
- **VERIFIED-RT (A/B test, 2026-11-02):** calling `EnableAI(false, true)` on a freshly spawned proxy left it **semi-transparent**. The same build with that call disabled rendered the proxies **fully opaque**, as confirmed by the user watching both windows. - **VERIFIED-RT (2026-11-02, freeze delayed until 3 s after the 3D loaded):** the proxy rendered opaque, but **its 3D stopped following the reference**. The reference's `data.location` kept tracking the remote player exactly (probe `(-749.1, 1653.6)` = network pose), while the visible model stayed where it was (user observation). So with AI disabled the engine stops updating the actor's 3D from its reference: no fade-in, no transform sync, no animation. `EnableAI(false)` therefore cannot be used on a live proxy at any point. The fade explanation for the transparency is consistent with this but not separately proven. The earlier "faint floating head" from cloning the player base (2026-10-27) was made with the same freeze. Its transparency is probably the same issue; why only the head was visible is UNKNOWN.

## 6. PlayerCharacter construction (can there be a second player?)

| Finding | Evidence | Confidence |
|---|---|---|
| `PlayerCharacter` RTTI: type descriptor `0x5BACF28`; vtables include `0x4CBBCD0` (COL offset `0x6A0`) and `0x4CBBD40` (`0x698`). | RTTI scan | VERIFIED |
| Its vtables are written in only two functions: `0x1A1D050` (ctor) and `0x1A1EAC0` (dtor; called by the scalar deleting dtor `0x1A1E9E0`). | xrefs to `0x4CBBCD0` | VERIFIED |
| The only caller of the ctor is `0x1891FB0`. It allocates the object, calls `0x1A1D050`, and **stores the result in the global `0x5F43230`**, which is `PlayerCharacter::Singleton` (Address Library ID 922868, the same ID as CommonLibSF's). It then initializes more globals (`0x61DF798`, `0x82D66B8..`). | `0x189201C..0x189215F` | VERIFIED |
| `0x1891FB0` is called from two places: `0x1883440` and `0x1896280`. | xrefs | VERIFIED (their roles are UNKNOWN; likely new-game/load) |
| `TESDataHandler::CreateReference` builds only `Actor` (two ctor overloads) for NPC bases and never constructs a PlayerCharacter. | §3 | VERIFIED |

Conclusion: the engine creates a PlayerCharacter only through a factory that overwrites the single player global. A second PlayerCharacter is not an engine-supported pathway. See REMOTE_PROXY_RESEARCH.md.

## 7. UI system

| Finding | Evidence | Confidence |
|---|---|---|
| The UI uses Scaleform GFx with ActionScript 3. | 317 `Scaleform::` strings in `.rdata` (e.g. `Scaleform::Render::HAL::PopMask`, `Scaleform AS3 Socket`), GFx SWF loader strings (`GFx_DefineBitsJpeg2Loader`), `.swf` suffixes. The menus ship in `Data/Starfield - Interface.ba2`. | VERIFIED |
| No Coherent GT / cohtml is present. | 0 hits for `Coherent`, `cohtml` | VERIFIED |
| `BSScaleformManager` singleton: ID 938002 → RVA `0x61DE670`. `UI` singleton: ID 937580 → `0x5FD9B80`. `UI::RegisterMenu`: ID 130463 → `0x2543AD0`. `UI::IsMenuOpen`: ID 130475 → `0x2544EC0`. | Address Library + CommonLibSF `IDs.h` | VERIFIED (addresses); behaviour per CommonLibSF only |
| `IMenu::LoadMovie` and the IMenu event handlers have **ID 0** in CommonLibSF for this runtime (unresolved). | CommonLibSF `IDs.h` | VERIFIED gap |
| SFSE 0.2.21 offers `SFSEMenuInterface` v2: `RegisterMenuMovieCreated(cb(IMenu*))` and `RegisterScaleformManagerCreated(cb(BSScaleformManager*))`. | `sfse/PluginAPI.h` | VERIFIED (header) |
| `EnableAI` (`0x1959170`) dereferences `UI` singleton `0x5FD9B80` (calls `0x2542020`, `0x2544220`) when re-enabling AI. | §5 | VERIFIED; purpose UNKNOWN |

## 8. Animation graph variables (locomotion)

Runtime evidence. Variables were read with Papyrus `GetAnimationVariable*` on the local player while it moved, and on the proxies. Candidate names came from `.rdata` strings.

| Variable | Exists on humanoid graphs | Meaning (calibrated on the player) |
|---|---|---|
| `Speed` (float) | yes | horizontal speed in world units/s: strafe 5.00 at a measured 5.16–5.36 u/s; backwards 4.40 at 4.38 u/s; 0 idle |
| `Direction` (float) | yes | movement direction relative to facing, as a fraction of a turn: 0 forward, 0.25 right, 0.5 back; holds its last value when idle |
| `TurnDelta` (float) | yes | 0 while walking straight; a stale value (-19.9) on a frozen proxy |
| `DirectionSmoothed` | on one of the two graphs only | |
| `SpeedSampled`, `SpeedSmoothed`, `IsMoving`, `IsRunning`, `IsSprinting`, `LocomotionBlend`, `LocomotionType`, `bInJumpState` | **no** | Papyrus.0.log: "cannot fetch variable named …" |

- `SetAnimationVariableFloat` writes stick on the proxy, with AI on or off: the readback equals the written value (Speed 4.3–5.2, Direction 0–0.5).
- **But writing `Speed`/`Direction` does not make the proxy play a walk cycle.** With AI on, it follows the position updates in a standing pose (user observation). NPC locomotion clips are therefore selected by the movement/AI system, not by these variables alone. **UNKNOWN:** which engine path selects NPC locomotion (the movement controller vs. graph events). That is the next RE target for animation.
- With AI on and no network motion, the proxy's own AI sets `Speed` 0.4–0.9 (it wanders), so the engine owns these variables whenever the AI moves the actor.
- **Resolved in [ANIMATION_REVERSE_ENGINEERING.md](ANIMATION_REVERSE_ENGINEERING.md):** `Speed`/`Direction`/`TurnDelta` are in the engine's per-frame variable cache (A.3). Stance, aim and jump variables are bound to AI-process memory (A.4). Locomotion is produced by the actor's `MovementControllerNPC` (motion- vs animation-driven, A.5), which `SetPosition` bypasses.

## 9. Appearance: `Actor.CopyAppearance`

| Item | Evidence | Confidence |
|---|---|---|
| `Actor.CopyAppearance(Actor akSource)` is registered in the Actor native table `0x1FCC740`; thunk `0x1FCC700`. | `lea rdx,"CopyAppearance"`, `lea r9,0x1FCC700` | VERIFIED |
| Thunk: if the source is None → "Cannot copy actor if source Actor is None". Otherwise it calls `0x17A96D0(self, source->data.objectReference /*+0x98*/, source == [0x5E61438])`. | disassembly; `0x5E61438` = Address Library ID 883420 (meaning UNKNOWN, a special actor, likely the player) | VERIFIED |
| `0x17A96D0` (ID 97401): `dst = self->base`. If `dst != src`, it calls `TESNPC::CopyAppearance(dst, src, 0)` (`0xCD8DF0`), then vfunc 0x17 on `dst` with `0x800` and `0x4000` (INFERRED: add save change flags). If the source is the special actor, it writes `dst+0x348 = src`. Finally it calls `0x195D720(self, 0, 0x28, 0)` (INFERRED: rebuild 3D/appearance). | disassembly | VERIFIED (semantics INFERRED) |
| **The copy targets the base NPC.** Every actor that shares that base changes, and the change is saved. | same | VERIFIED |
| `TESNPC::CopyAppearance` (`0xCD8DF0`) copies: ACBS flag bit 0 (`+0xF0`, female) through `0x5D58A0`; byte `+0x450`; ACBS flag bit 19 through `0xCCE940`; 12 bytes at `+0x350..+0x35B`; 5 floats from the array at `+0x3D8` through `0xCE7BB0(dst, i, v)`; the bulk through **`0xCD56E0(dst, src, flag)`**; byte `+0x400`. | disassembly | VERIFIED (field meanings UNKNOWN until `0xCD56E0` is decompiled) |

Consequence: an exact copy of a remote player means sending these fields over the network and applying them to a **proxy-owned** base NPC, never to a shared base. The local player's base `00000007` would change the local player itself.

## 10. Crash evidence not yet analysed

- Four crash dumps from 2026-10-27, from the cell-walking experiment: `Starfield_10-28-03-{07,13,20,24}.dmp` (~150 MB each) in the game folder. The commit history attributes the 22:20/22:24 silent exits to iterating `TESObjectCELL::references` and reading `Actor::boolBits` with unverified layouts. That code was removed. The dumps have **not** been analysed (no WinDbg/cdb installed). **UNKNOWN** until analysed.

## 12. NPC state, actor update and control handover

All findings below come from disassembly and decompilation (`tools/re/sfre.py`, `tools/re/ida/decompile_list.py`). **None has been exercised at runtime yet** unless marked VERIFIED-RT. All of these run on the game's main thread in CoSync (`GameBridge::Tick`), and only their readers and callers were examined, not every thread that touches these fields, so concurrent access is UNKNOWN.

### 12.1 State reads, from the Papyrus natives

Natives were found at their registration sites, where the pointer is loaded into `r9`. `natives.py` misses this template family. Actor natives are registered around `0x1FCC740`. A second block of same-named natives near `0x1F2xxxx` appears to belong to SpaceshipReference (INFERRED).

| Script function | Native | What it does | Confidence |
|---|---|---|---|
| `Actor.IsDead` | `0x1FC7FB0` | Actor vfunc 269 (`vtbl+2152`) with `dl=1` | VERIFIED |
| `Actor.GetCombatState` | `0x1FC6DE0` | vfunc 363 (`vtbl+2904`) false: 0. Then vfunc 351, field `+128` non-zero: 2, else 1 | VERIFIED |
| `Actor.IsWeaponDrawn` | `0x1FD1490` | `(byte[actor+252] & 7) >= 3` | VERIFIED |
| `Actor.IsInScene` | `0x1FD1330` | `dword[actor+520] & 8` | VERIFIED |
| `Actor.IsSneaking` | `0x1FD1420` | `0x18E7660` (ID 100547) | VERIFIED |
| `Actor.GetSitState` | `0x1FC81F0` | vfunc 52 of the subobject at `actor+240`, returns 0..4 | VERIFIED |
| `ObjectReference.IsInDialogueWithPlayer` | `0x20761F0` | For an actor, `dword[actor+892] & 8` | VERIFIED |
| `ObjectReference.GetValuePercentage` | `0x20737C0` | `actor+112` ActorValueOwner: slot 1 / slot 2, or 1.0 when slot 2 is 0 | VERIFIED |
| `ObjectReference.DamageValue` | `0x206B300` | Owner `vtbl+56` (slot 7) with modifier 2 and a negative delta, then type-5 handling on reaching 0 | VERIFIED |
| `Actor.SetRestrained` | `0x1FCA3C0` | Refuses if dead. `0x191C2F0`: vfunc 367 with life state 6 | VERIFIED |
| `Actor.SetUnconscious` | `0x1FCA5F0` | Life state field `dword[actor+248] & 0x1E0000`; `0x60000` means unconscious | VERIFIED (meaning from the function name) |

- Health: `ActorValue::GetSingleton` (ID 43134) is `0x49A430`, the function `DamageValue` compares against, and its `+264` matches CommonLibSF's `health` at `+0x108`. Two independent sources agree. VERIFIED.
- CommonLibSF declares two overloads of `ModActorValue` at slots 6 and 7. CoSync calls slot 7 through the vtable, because MSVC can order overloads differently from the source. Only the slot used by `DamageValue` is relied on.
- **Correction to A.12:** vfunc 363 is the "is in combat" test: `GetCombatState` uses it as the first check. A.12 described only what it reads. Its behaviour is unchanged.

### 12.2 The AI bit gates the whole actor update

`0x18E8E50` is the actor's per-tick update (INFERRED from its body; 9 call sites, among them `0xB4B02F` and `0x1A59819`). Its entry reads, abridged:

```
if (formFlags & 0x800 /*disabled*/ || !(actor[+520] & 2) /*AI enabled*/ || ...) { ...; goto end; }
```

- Everything else in the function sits behind that test (VERIFIED), including calls into the process object (`0x19B28C0`, `0x19C9950`, `0x19C3B50`, `0x1A107B0`) and `0x1911510`, `0x1911E70`. That these are the AI, package and movement updates is INFERRED; each call has not been traced.
- `Actor::EnableAI` (`0x1959170`) also calls `0x23B20F0` on the character controller (`process+8 -> +0x458`) with the flag, and re-registers something at `process+0x10 -> +640`. VERIFIED instruction-level; purpose UNKNOWN.
- **Consequence:** this explains the frozen 3D of §5, though it is not separately proven which of the two causes it. Either way, the AI bit cannot separate "decide" from "move and render". A client cannot keep an NPC visible with AI off. INFERRED.
- Other readers of the bit: `0x1A83C40`, `0x195ACD0`, `0x1939630`, and the native at `0x1FD1181` (`IsAIEnabled`-shaped). Roles UNKNOWN.
- **Package evaluation:** `Actor.EvaluatePackage` is native `0x1FC5E70` -> `0x1914750(actor, 0, flag)`, refused when the form is deleted or disabled, and calling `0x19146F0`. It is the candidate hook if NPC decisions must be suppressed. Not tested, and whether all evaluation goes through it is UNKNOWN.

### 12.3 Reversing the player-controls state

`MovementControllerNPC` vtable: ID 452731, `0x4CC0DB0`.

| Slot | Function | Effect |
|---|---|---|
| 7 | `0x1AB7D80` (ID 103566) | Selects the default: mode 2 gives state 7, 8, 12 or 2 by flags `+197/+198/+199`; otherwise state 10 |
| 8 | `0x1AB7E60` | State 3 or 11 |
| 9 | `0x1AB7EB0` (ID 103568) | State 1, player controls |
| 11 | `0x1AB7F00` | State 0 |
| 12 | `0x1AB7F90` | State 0 |
| 10 | `0x1AB8040` | State 13 |

All VERIFIED by decompilation. Slot 7 is the reverse of slot 9 at the state level. That it fully restores an NPC, including agents and any registrations made while in state 1, is **UNVERIFIED** until tests 11 and 12 of the status doc.

Runtime 2026-11-04 (VERIFIED-RT): real NPCs leave state 1 by themselves, mostly into state 4. Slot 13 (`0x1AB7FC0`) sets 4 or 9 and slot 14 (`0x1AB8110`) sets 5. CoSync's controller hold hooks slots 7, 8 and 10 to 14 and returns 0 for held controllers, the value of a failed `0x2735FB0`. Whether callers cope with that refusal is UNVERIFIED. `0x1AB80D0` also writes state 4 and is not one of the hooked slots, so it may bypass the hold.

`LeavePlayerControlsState` also restores `IMovementPlayerControls` to `PlayerControls::Manager+0x20` when the controller is motion-driven, because vfunc 16 registers exactly that (A.5).

### 12.4 Dialogue, scenes, quests, faces: first survey

Static survey only: class names come from RTTI, so they prove a system exists, not how it behaves. Nothing here has been tested.

| Area | Found | Confidence |
|---|---|---|
| Dialogue classes | `MenuTopicManager` (vtables `0x4CA81D0/E8`, `0x4CA8200`; singleton ID 938421 per CommonLibSF), `DialogueMenu`, `DialogueMenu_OnDialogueSelect` (`0x4D08378`), `TESTopic`, `TESTopicInfo`, `DialogueCameraState`, `SubtitleManager`, `BGSProcedureForceGreet` | VERIFIED (RTTI) |
| `ObjectReference.Say` | `0x207EF20`: refuses a None topic info, then `0x205EC50(self, topicInfo, 0, speakAs, target)`. Argument meanings INFERRED | VERIFIED |
| `Actor.IsTalking` | `0x1FD1450`: Actor vfunc 104 (`vtbl+832`) | VERIFIED |
| Scene classes | `SceneManager`, `BGSScene`; actions `Dialogue`, `PlayerDialogue`, `Animation`, `Camera`, `FX`, `Move`, `Package`, `Radio`, `StartScene`, `Timeline`, `Timer`, `List` | VERIFIED (RTTI) |
| `Scene.Start` | `0x1FFD320`: refuses unless the parent quest (`scene+168`) is running (`quest+276` bit 0 set, bit 7 clear, `+824` null, `+812` zero), then sets `scene+80` bit 0 and calls `0xCB6010`. The native name is INFERRED from its neighbours and its error text | VERIFIED |
| `Quest.IsStageDone` | `0x20CB530`: stage index must fit 16 bits, then `0xD0DBD0(quest, stage)` | VERIFIED |
| Story manager | `BGSStoryManager*` tree classes, `BGSStoryTeller`. The singleton ID is unresolved in CommonLibSF | VERIFIED (RTTI) |
| Faces | `BSFaceGenAnimationData`, `BSFaceFX`, `BGSFaceFX`, `BGSFacialExpressionData`, `PerformanceMorphWeights` | VERIFIED (RTTI) |
| Expression override | `ClearExpressionOverride` `0x1FC5660`: needs a process and face data from `0x19FA160`, then `0x18E0650` | VERIFIED |
| Look at | `ClearLookAt` `0x1FC56F0`, which writes the invalid handle to `process+0x10 -> +652` | VERIFIED. Field meaning INFERRED |
| Graph handlers | `HeadTrackingOn/OffHandler`, `HeadTrackingRotationOn/OffHandler`, `EyeTrackingOn/OffHandler`, `FaceEmotionalIdleHandler`, `GetUpStart/EndHandler`, `RagdollStartHandler`, `KnockdownHandler`, `StaggeredStopHandler`, `IdleDialogueEnter/ExitHandler` | VERIFIED (RTTI). That they react to named graph events is INFERRED |
| Physical | `StaggerMeter` (`0x4D53E20`), `StaggerEffect`, `RagdollGetUp`, `AnimationRagdoll`, `BGSBleedoutActivity` | VERIFIED (RTTI) |

### 12.5 Engine events

319 distinct `BSTEventSink<…>` types exist in the binary, among them `TESDeathEvent`, `TESEquipEvent`, `TESTopicInfoEvent`, `TESQuestStageEvent`, `TESQuestStartStopEvent`, `TESSceneEvent`, `TESScenePhaseEvent`, `TESSceneActionEvent`, `TESFurnitureEvent`, `TESExitFurnitureEvent`, `TESPackageEvent`, `TESCombatEvent`, `TESOpenCloseEvent`, `TESActivateEvent`, `TESContainerChangedEvent`, `TESOnPCDialogueTargetEvent`, `TESEnterBleedoutEvent`, `BGSActorDeathEvent` and `AliasChangeEvent`. VERIFIED (RTTI).

**Their sources are mostly unresolved.** CommonLibSF has IDs for only a few here: `TESEquipEvent` 64135, `TESLoadGameEvent` 64149, `TESBookReadEvent` 64108, `ActorCellChangeEvent` 64106. The rest are ID 0.

### 12.6 Failed attempt: deriving event sources statically

Tried on 2026-11-04 and **not usable**, recorded so it is not repeated:

- Matching `RegisterSink` (`0x22C8E80`) call sites to RTTI base offsets resolved only 28 events and **none of the four known getters**, so it could not be validated. Its candidates, such as `0x4EC960` (ID 44207) for `TESDeathEvent`, are **UNVERIFIED**, and they must not be used as facts.
- Reading offsets from `0x1FB8B60`, which calls 41 getters including the Equip one, **failed its control**: `0xB99C40` came back as `BGSLocationLoadedEvent`, not `TESEquipEvent`. The offsets in that function do not identify sinks.

What did hold: getters are thread-safe static-local functions with one shape (`sub rsp,28; mov rax,gs:[58]; …; lea rax,<source>`). The proposed next step is a runtime probe that registers a counting sink on a candidate getter and reads no event fields, then checks whether the count rises only when the event should happen.

### 12.7 Runtime findings, 2026-11-04

- `ProcessLists` holds at least four actor-handle arrays: `+0x60` (64 entries), `+0x70` (1173), `+0x90` (35), `+0x190` (5). All entries resolved as actors. VERIFIED-RT. What each list means is UNKNOWN. Static analysis of `ProcessLists` users gave no offsets.
- The controller hold works: over 15 minutes, no NPC left state 1 while 16,400 state changes were refused, without a crash. VERIFIED-RT.

### 12.8 Menu pause

- `0x1888410` (ID 99406) is the main frame update. Each frame it sets `Main+0x448` to 1 if `UI+0x4B4` is non-zero, `MainMenu` is open (`0x2544D60` with the name from `0x253D330`), or global `0x5FB4B78` is set. VERIFIED. `Main` singleton: ID 883591, global `0x5E71710`.
- 13 functions read `Main+0x448`. About 80 sites read `UI+0x4B4` directly, so those systems may still stop during menus after the patch. UNKNOWN which.
- Patch (`MenuPause.cpp`): the `ja` at `+0x64` is removed, and the call at `+0x71` is redirected to a hook that restores the pause if a `LoadingMenu` is open or no session is active. Whether `LoadingMenu` covers every unsafe case is UNVERIFIED.

### 12.11 Engine events, doors, quests and the console

From IDA 9.3 and from runs of `DevHarness.cpp` on 2026-11-07. VERIFIED-RT below means the event was seen in two running copies with its payload decoded.

**Event getters.** IDs 64095 to 64237 are one block of 142 global getters, 0x70 bytes apart, all `sub rsp,28h` (ID 64114 has no address). A generic sink registered on all of them, 142 installed and 0 refused. This replaces the failed approach in 12.6. Position in the block is not evidence of which event a getter is: the order is close to alphabetical but not exact, for example quest timers are far from quest stages. Each below was identified by what fired after a scripted action, or by its payload.

| ID | Event | Payload | Confidence |
|---|---|---|---|
| 64118 | `TESContainerChangedEvent` | u32 from, to, item, count | VERIFIED-RT: `additem` gave `0, 0x14, 0xF, 5`, `removeitem` swapped from and to |
| 64160 | Quest stage | pointer, then u32 quest, stage | VERIFIED-RT payload, name INFERRED |
| 64161 | Quest stage done | u32 quest, stage | VERIFIED-RT payload, for example `002C9C96, 100`. Name INFERRED |
| 64162 | Quest start or stop | u32 quest | INFERRED |
| 64153 | `TESOpenCloseEvent` | ref, activator, byte | INFERRED: door `000B2B04` with the flag going 1 then 0 |
| 64099 | `TESActorActivatedRefEvent` (12.12) | ref, ref | VERIFIED: RTTI |
| 64179, 64180 | `TESTriggerEnterEvent`, `TESTriggerLeaveEvent` (12.12) | ref, actor | VERIFIED: RTTI. Earlier read as furniture, which was wrong: furniture is 64139 and 64101 |

`RE::Activation::Event` (ID 63133, `source, target`) is VERIFIED-RT and is what WorldSync watches. `BSTGlobalEvent::EventSource<T>` has 525 RTTI entries, mostly UI, and the `TES*` events above are not among them.

**Doors.** `0x5CDC70` (ID 47134) returns 1 open, 2 opening, 3 closed, 4 closing, VERIFIED-RT. Its switch accepts base types 0x1E, 0x24, 0x25, 0x27, 0x2F, 0x3E. `ObjectReference.SetOpen` is `0x2080BC0`: it activates through `0xB47920` if the door is not already there, then sends the graph event `Open` or `Close`.

**Quests.** `Quest.IsRunning` `0x20CDCA0`: `quest+0x114` bit 0 set, `0x80` clear, `+0x338` null, `+0x32C` zero. `Quest.IsActive` `0x20CDC20`: bit `0x800`, which proved to mean the one tracked quest (1 of 269 running), VERIFIED-RT. Objectives: count `+0x148`, array `+0x150` (`0xD09230`), state at `+40`, shown when 1, 3 or 5 (`Quest.IsObjectiveDisplayed` `0x20CB700`). Stages: count `+0x138`, array `+0x140`, index at `+16`, done bit at `+18` (`0xD0DBD0`, ID 68530, VERIFIED-RT). CommonLibSF has ID 0 for `TESQuest::IsStageDone`, and calling it froze the main thread. CommonLibSF's `QUEST_DATA` flags at `+0x10C` are not what these read. Stages are set through `Quest.SetCurrentStageID`. The console text says setting a stage starts a stopped quest (`Stage %d set (quest will be started first)`), which is why WorldSync refuses that.

**Weather forms** are `BGSWeatherSettingsForm` (form type `0x44`, 181 of them), not `TESWeather`. Packing one as a script object fails, so `Weather.SetActive` could not be called this way, while the console's `fw <id>` works, VERIFIED-RT.

**Locks.** Event getter ID 64148 fires on every lock change, payload word 0 the reference, VERIFIED-RT (it fired once for each `Lock` call on `000B2B04`, and for changes no test made). Lock data comes from ID 64290 (`0xBA0610`, used by `ObjectReference.IsLocked`): bit 0 of `+12` is locked, `+13` the level, and `null` means no lock. `Lock` (`0x2076B40`) creates the data on a reference that had none, VERIFIED-RT.

**Destruction stage event.** `TESDestructionStageChangedEvent` has a function-static source returned by getter `0xB997E0` (ID 64125, inside the global getter range). It is raised by `0xB97470` (ID 64056) with the payload `{ reference, old stage, new stage }`: a smart pointer to the reference, then two 32-bit stages. Found from the source's type name `.?AV?$BSTEventSource@UTESDestructionStageChangedEvent@@@@` (vtable `0x4B982F0`, object `0x5977CC0`). DECOMPILED; not seen firing, because the test save has nothing breakable.

**Destruction.** `ObjectReference.GetCurrentDestructionStage` calls ID 47007 (`0x5C39A0`), which **crashes** on a reference whose base has no `BGSDestructibleObject_Component`: it reads `[component+8]` without a check (dump `Starfield_11-07-20-07`, `Starfield.exe+0x5C3A17`). Check first with ID 47392 (`0x5DDB20`, find a component by key) and key ID 46986 (`0x5BF6D0`). Destroyed is bit `0x800000` of the form flags at `+0x20` (`ObjectReference.IsDestroyed`).

**Ships, first findings.** Ship references are ordinary `REFR` (type 74) whose base has form type 65 (`BGSGenericBaseForm`) and flag `0x40`, the test `0x1FA78D0` makes. 108 exist in the New Atlantis save, most in sky cells. `Game.GetPlayerHomeSpaceShip` takes no arguments (`NativeFunction0`). `Actor.MoveTo` refuses a target in a sky cell ("non-unique cell ... can be instanced", Papyrus log). Boarding a landed home ship through `MoveTo` puts the player in its interior (`BlueprintShips-Starfield.esm:00100136`), VERIFIED-RT. `SpaceshipReference.TakeOff` (`0x1F220B0`) calls ID 119907 (`0x21199B0`), which has the pilot perform the take-off action. With the player seated as pilot it returned 0 and the ship stayed landed. The cockpit menu's Launch handler is ID 93833 (`0x16757D0`). Called with a stand-in menu, it ran without taking off. Both are open questions.

A save with the home ship parked in space (cell `00018343`) gets round take-off: boarding it there puts the player aboard with `ObjectReference.IsInSpace` (ID 63482, `0xB58D50`) true for both ship and player, VERIFIED-RT. `SpaceshipReference.TakeOff` checks the same function first and does nothing in space.

**Placing a stand-in ship.** `ObjectReference.PlaceShipAtMe` (`0x207A8B0`) takes the base, a level mod, four flags, an offset array (0, 3 or 6 floats), two more values and a last flag. It refuses an anchor that is "not a spaceship landing marker or in space", and an anchor ship already in use. In space it takes a separate path (`0x1F56A10`) that seems to finish later. Anchored on the home ship the call never returned. Anchored on a static in the same space cell it returned None at once. A stand-in ship has not been placed yet.

**The console entry** (ID 113576) runs commands without a target (`fw`, `LoadGame`), but `player.moveto` and `set gamehour` do nothing, and `tots`/`lopa` did nothing either. Its output does not reach `ConsoleLog`'s buffer.

**Time and weather.** `Utility.GetCurrentGameTime` reads `Calendar+0x30`'s value at `+0x48`, matching CommonLibSF. Writing `gameDaysPassed` and `gameHour` moves the clock and it keeps running, VERIFIED-RT. `Weather.GetCurrentWeather` is `Sky+0x118`, global `0x61DF6F0` (ID 938081).

**Console.** `StartingConsoleCommandRunner` calls `0x1EA1E10` (ID 113576) with global `0x61E26B0` (ID 938528) and a command used as a format string. Called from the main menu with `LoadGame <name>` it loads the save, VERIFIED-RT. `set gamehour to N` through it does nothing, cause UNKNOWN.

**Cells.** Cell `+0x88` points at the reference array, 1655 entries in `000125B6`, VERIFIED-RT. That is the data pointer of CommonLibSF's array at `+0x80`, so the header is right. Walking it through `ForEachReference` crashed reading address `0x22` from a value of 2 (dump `Starfield_11-07-17-39.dmp`), cause UNKNOWN. Base form type values and CommonLibSF's `FormType` agree, checked against RTTI names at runtime. Many references in an exterior cell are disabled, 444 of 1655 there.

**Lists.** `TESDataHandler::formArrays[kQUST]` reads as empty on 1.16.244, so it cannot be used to list quests.

### 12.12 Names of all global events

The caution in 12.11 that position is not evidence is answered by a better method. Each getter from ID 64095 to 64237 returns a function-static `BSTEventSource<T>`. The object sits in `.data` with its vtable already written, and that vtable's RTTI names `T`. `tools/re/eventnames.py` prints the table. Controls: 64125 and 64118 come back as the names previously proven at runtime (`TESDestructionStageChangedEvent`, `TESContainerChangedEvent`). VERIFIED on 1.16.244. This supersedes 12.6.

| IDs | Event |
|---|---|
| 64098, 64099 | `TESActivateEvent`, `TESActorActivatedRefEvent` |
| 64100 | `BGSAffinityEventEvent` |
| 64101, 64139 | `TESExitFurnitureEvent`, `TESFurnitureEvent` |
| 64102 to 64104 | `TESActiveEffectApplyFinishEvent`, `TESActiveEffectRemovedEvent`, `TESPlayerActiveEffectEvent` |
| 64105, 64106 | `TESActorLocationChangeEvent`, `ActorCellChangeEvent` |
| 64109 to 64113 | Cell reference attach and detach, 3D attach, critical references, fully loaded, navmesh generated |
| 64115 | `TESCellGravityChangeEvent` |
| 64116, 64117 | `TESCombatEvent`, `TESCombatListEvent` |
| 64119, 64120 | `CrewAssignedEvent`, `CrewDismissedEvent` |
| 64121 | `BGSLocationLoadedEvent` |
| 64122 to 64127 | `TESDeferredKillEvent`, `TESDeathEvent`, `DestroyedEvent`, `TESDestructionStageChangedEvent`, `TESEnterBleedoutEvent`, `TESExitBleedoutEvent` |
| 64128 | `TESEnterSneakingEvent` |
| 64135 | `TESEquipEvent` |
| 64136 | `TESPlayerFollowerWarpEvent` |
| 64140 | `TESGrabReleaseEvent` |
| 64141 | `TESHitEvent` |
| 64142, 64144 | `HomeShipSetEvent`, `PlayerModifiedShipEvent` |
| 64147, 64148 | `TESLocationExploredEvent`, `TESLockChangedEvent` |
| 64150 | `TESMagicEffectApplyEvent` |
| 64154 | `TESPackageEvent` |
| 64157 to 64163 | Quest init, mission accepted, quest rejected, stage, stage item done, start and stop, `AliasChangeEvent` |
| 64164 | `TESResetEvent` |
| 64166 to 64168 | Scene, scene action, scene phase |
| 64169 | `TESSellEvent` |
| 64170, 64171, 64182, 64183 | Sleep start and stop, wait start and stop |
| 64174 | `TESTopicInfoEvent` |
| 64177 to 64180 | Trap hit, trigger, trigger enter, trigger leave |
| 64185, 64186, 64236 | `TESConsciousnessEvent`, `TESLimbCrippleEvent`, `TESUnconsciousEvent` |
| 64189 | `BGSRadiationDamageEvent` |
| 64190 to 64204 | Player events: heal teammate, loitering, companion dismiss, long falls, mod armor or weapon, research complete, craft, robots, **fire weapon (64200)**, workbench, speech challenge. 64201 and 64202 (swimming, vertibird) are left over from earlier games |
| 64205, 64206 | `PlayerAddItemEvent`, `ActorValueChangedEvent` |
| 64207, 64208 | `BGSCellGridLoadEvent`, `BGSHavokWorldCreatedEvent` |
| 64209 | `ReferenceDestroyedEvent` |
| 64212 to 64215 | Builder menu select, planet survey complete, survey progress, object scanned |
| 64217 | `MapMarkerDiscoveredEvent` |
| 64218 to 64224 | Crime: arrested, assault, murder, crime gold, jail, pay fine, trespass |
| 64225, 64226 | Buy ship, sell ship |
| 64228 to 64230 | Item swapped, lock picked, item consumed |
| 64232 | `RefControlChangedEvent` |
| 64237 | `CelestialLocationDiscoveredEvent` |

**Runtime notes.** `TESHitEvent` 64141 fired during fights: its first three floats are the world position of the hit, matching the position of the actor struck. The rest of the layout is not decoded. `TESCombatEvent` carries actor, then target. `TESLimbCrippleEvent` carries actor, then source, then the limb's actor value (type 109, `AVIF`).

### 12.13 Death and saving

All on 1.16.244, decompiled with IDA 9.3, and exercised in two running copies on 2026-11-08 unless noted.

| Item | Address | Finding | Confidence |
|---|---|---|---|
| Kill dispatcher | ID 100769, `0x1906010` | `Actor.Kill` reaches it, and it calls vtable slot 386 | VERIFIED-RT |
| Death slot | vtable +3088, slot 386. `Actor` `0x1906250` (ID 100770), `PlayerCharacter` `0x1A3CC50` (ID 102723) | **Lethal damage from `DamageValue` reaches this without passing the dispatcher.** Hooking only the dispatcher left the player in life state `0x20000` and the save reloaded about 4 s later | VERIFIED-RT |
| Life state setter | vtable slot 367. `Actor` `0x190BFF0` (ID 100787), `PlayerCharacter` thunk `0x1A1FF10` | Argument 2 is the state. 1 and 2 are the two death states, 7 is bleedout | VERIFIED (decompiled). Which of the two player slots stops a given death is not separated |
| Death states | `Actor+0xF8 & 0x1E0000` | `0x20000`, `0x40000` | VERIFIED-RT |
| Death reload tick | ID 102812, `0x1A59040` | Player per-frame function that tests those states. That it starts the reload countdown comes from PULSAR's notes | Partly verified |
| `Game.SetInChargen(disableSaving, disableWaiting, showMessage)` | Native `0x20149C0` (ID 117497) | Sets bits 0, 1, 2 of the byte at `PlayerCharacter+0xF64` | VERIFIED (disassembly, pex signature) |
| Save gate | ID 98657, `0x1846EF0`, `bool (manager, flag)` | Refuses if bit 0 of `+0xF64` is set, or the player is in a death state, among other tests. CommonLibSF calls this `IsLoadingAllowed`, which is misleading. More than 20 callers, including the function that uses the `Autosave` string (`0x183C590`) | VERIFIED-RT (answered "refused" with the bit set) |
| Save manager | ID 883588, global `0x5E6D6E8` | First argument to the gate | VERIFIED-RT |
| Moving the player | `Actor::SetPosition`, vtable slot 0x137, `PlayerCharacter` `0x193B410` | Moves the player with no loading screen, across cell borders on generated terrain. Exact at 190 m. A 500 m move ended about 217 m along, at the same spot on both copies. Cause UNKNOWN, probably unloaded cells. Papyrus `SetPosition` and `MoveTo` always open `LoadingMenu` for the player | VERIFIED-RT |

### 12.9 Not yet investigated

Internals of dialogue flow, scene execution, quest scripts, equipment, ragdoll, loot, and actor attach and detach. Sections 12.4 and 12.5 list entry points only.

### 12.10 CrowdManager, the ambient crowd

Decompiled with IDA 9.3 on 2026-11-06. **Nothing here has been exercised at runtime** unless marked VERIFIED-RT.

Singleton: `CrowdManager*` at global `0x5FD9B70` (ID 937582). Constructor `0x1272750`, destructor `0x1272F70`.

| Manager offset | Meaning | Evidence |
|---|---|---|
| `+0x48` (72) | Crowd movement processing, copied to each entry's `+0x70`. Console toggle `0xC658A0` | VERIFIED |
| `+0x49` (73) | Crowd animation processing, gates the per-actor jobs in `0x127D290`. Console toggle `0xC65A70` | VERIFIED |
| `+0x4A` (74) | Gates the update `0x127E900` | VERIFIED |
| `+0x50` | Read-write lock | VERIFIED |
| `+0x68` / `+0x70` | `CrowdActorInfo` count and data, 0x80 bytes each | VERIFIED |
| `+0x78` / `+0x80` | Target table count and data, 80 bytes each. `+56` handle, `+60` occupant, `+64`, `+68`, `+72` links | VERIFIED (`0x1278150`, `0x1271B20`) |
| `+0x329` (809) | Inhibit strategy: 1 at `+824` Disable, 2 at `+832` Default, otherwise `+816` Null | VERIFIED |

`CrowdActorInfo`: `+0x00` actor handle, `+0x04` target handle, `+0x08` previous target, `+0x0C` queued target, `+0x18` region, `+0x40` inhibit level, `+0x70` movement copy, `+0x72` near the player, `+0x74` inhibited, `+0x78` state. All VERIFIED, roles of `+0x08` and `+0x0C` INFERRED from their use.

**The update.** Per frame `0x17CAB70` calls `0x127D290`, which queues jobs per actor: `0x1272610` -> `0x1271140` -> `0x127E900` -> `0x127EBA0`, the state machine. `0x127EAEA` is its only call site, and it sets only `rcx`, `rdx` and `r8`. It runs on job threads.

| State | Does | Next |
|---|---|---|
| 0 | Builds a `PathingRequest` to the target, issues it with `0x19229A0` | 1, or 12 if seated |
| 1 | Walks. On arrival, `0x19FFB10` for furniture | 2, 11, 14 or 7 |
| 2 | Arrived. Random wait from the standing or furniture timers | 4, 5 or 6 |
| 3, 4, 6 | Timed waits | 7 |
| 7, 9 | Clears the target with `0x1271B20` | 8, 10 |
| 8, 10 | **Picks the next target at random**, through `0x127D580` | 0 |
| 11, 12 | Sitting down, standing up | 2, 1 |
| 14 | Leaves through a door | 15 |
| 17 to 21 | Reacts to the player | 7 |
| 23 | Scripted path | 0, 1 or 12 |
| 25 | Dead | |

Meanings of states are INFERRED from their bodies; the transitions are VERIFIED.

`0x1271B20(manager, entry, handle)` is SetTarget: it clears the old target's occupant, sets the new one's, stores the handle at `+0x04` and the old one at `+0x08`. `0x12718E0(entry, state)` is SetState. Issuing a new path from state 1 is something the machine does itself (repath), and state 0 stands a seated actor up (`v110 = 12`). VERIFIED.

**Randomness cannot be seeded.** `0x4071D0`, `0x2FCF60` and `0x2FBE70` all draw on one engine generator at `0x591F4D0`, also used by about 20 unrelated functions (`0x15A3350`, `0x2150520`, `0xDDEA60`, others). Together with job threads, the same seed cannot give the same result. Spawning (`0x1276A70`, `NEW_REFR_DATA`) uses the same generator. VERIFIED. That two copies of one save start with an identical crowd is VERIFIED-RT (2026-11-05, same positions, clothes and looks, other FormIDs); they then diverge.

**Inhibition is per camera.** `0x1275360` runs on the main thread. While `bCrowdInhibitEnable:AI` (value `0x5F10770`) is on, strategy = `(crowd size > uCrowdInhibitThresholdMinActor) + 1`, compared unsigned, default threshold 80 (setting at `0x5F107A8`, value `0x5F107B0`, ID 914896). A change calls the new strategy's enter and update slots. Default (2) sets entry `+0x74` from occlusion and distance. Such an actor skips `0x127E900` when `0x1286C40` agrees, so it stands still on that copy. Disable's update `0x126D100` clears `+0x74` on every entry. VERIFIED. 101 crowd actors were measured, so Default was active.

**Settings** hold their value at object `+8` (`0x305FD0`). 90 or so `f/b/uCrowd*` settings exist, mostly in sections `Crowd` and `AI`. `bCrowdSystemEnabled:AI` (`0x5F106D0`) and `uMaxCrowdActorCount:Crowd` (`0x59169E0` table, default 250) are the coarse controls. Not used: turning the crowd off is not synchronising it.

**What CoSync does** (`CrowdSync.cpp`, EXPERIMENTAL):

1. Raises the threshold to `0xFFFFFFFF` during a session on every copy, so the engine itself selects Disable. The authority's camera can no longer stop an actor. Restored on leaving. Cost: more active crowd actors, frame rate unmeasured.
2. The authority publishes each actor's target as a **position**, since a target may be a runtime reference numbered per copy. Whether any are is UNKNOWN; the log now counts misses.
3. A replica finds the local target at that position (within 0.25) and hooks `0x127EAEA`. On a different target it calls SetTarget and SetState 0, exactly as state 8 does. With the same target it skips the update in states 3, 4, 6 to 10, where the machine would wait or choose at random. States 13 to 23 and 25 are never redirected.
4. A crowd actor with no local match, a door for example, stays on the old driven path.

Not solved: each copy still spawns and despawns its own actors (`0x1276A70`, `0x1281250`), so an actor spawned on the authority only pairs if a similar local one exists. Replicating the spawn itself needs its own work.

## 13. Open items (what is needed and why)

1. **IDA Pro 9.3** (now available; database being built under `local/ida/`, script `tools/re/ida/analyze_and_save.py`). Decompile `0xCD56E0` (appearance fields), `0x5A0520` (CreateReference), NPC locomotion selection, and `0x23B20F0` (the EnableAI → process path).
2. **WinDbg/cdb** to analyse the four crash dumps.
3. The full new-reference descriptor layout, if direct (non-Papyrus) spawning is pursued.
4. Animation-graph driving for proxies: the holder at `+0x60` is now verified (slots 0x0A/0x0D/0x12). See ANIMATION_REVERSE_ENGINEERING.md §A.9 for the remaining items.
