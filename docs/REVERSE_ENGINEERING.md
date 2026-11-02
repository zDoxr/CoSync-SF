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

- **IDA Pro and Ghidra are not installed** on this machine. IDA left only a config folder; there is no Ghidra and no Java runtime. The REA MCP server reports both providers as unavailable.
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
- **VERIFIED-RT (A/B test, 2026-11-02):** calling `EnableAI(false, true)` on a freshly spawned proxy left it **semi-transparent**. The same build with that call disabled rendered the proxies **fully opaque**, as confirmed by the user watching both windows. The precise mechanism is UNKNOWN. The leading hypothesis is that the fade-in of newly loaded 3D stops advancing once AI/animation updates are disabled; the `0x23B20F0` call path is the place to confirm this. The earlier "faint floating head" from cloning the player base (2026-10-27) was made with the same freeze. Its transparency is probably the same issue; why only the head was visible is UNKNOWN.

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

## 8. Crash evidence not yet analysed

- Four crash dumps from 2026-10-27, from the cell-walking experiment: `Starfield_10-28-03-{07,13,20,24}.dmp` (~150 MB each) in the game folder. The commit history attributes the 22:20/22:24 silent exits to iterating `TESObjectCELL::references` and reading `Actor::boolBits` with unverified layouts. That code was removed. The dumps have **not** been analysed (no WinDbg/cdb installed). **UNKNOWN** until analysed.

## 9. Open items (what is needed and why)

1. **IDA Pro or Ghidra** (Ghidra 12.1.x + JDK 21) to decompile `0x5A0520` (CreateReference), `0x23B20F0` (the EnableAI → animation path) and the 3D fade logic. The capstone tooling is enough for targeted reads, but not for type recovery across a 0x2000-byte function.
2. **WinDbg/cdb** to analyse the four crash dumps.
3. The full new-reference descriptor layout, if direct (non-Papyrus) spawning is pursued.
4. Animation-graph driving for proxies (`IAnimationGraphManagerHolder` at TESObjectREFR+0x60 per CommonLibSF; unverified).
