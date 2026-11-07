# CoSync-SF — NPC Synchronization Roadmap

**Objective:** two players exploring Starfield together genuinely experience the same living world, down to facial expressions, head tracking and weapon transitions. Position sync alone does not count.

Current state of each item: [NPC_SYNC_STATUS.md](NPC_SYNC_STATUS.md). Evidence: [REVERSE_ENGINEERING.md §12](REVERSE_ENGINEERING.md#12-npc-state-actor-update-and-control-handover).

## How the work is ordered

Each stage has a gate, tested in game on two clients. Later stages build on earlier ones, so a stage does not start until the gate before it passes. Compiling or passing unit tests never opens a gate.

The engine rule that shapes every stage: **find the engine's own event or call for a behaviour, observe it on the authority, and trigger the same thing on the client.** Copying results, such as writing animation variables, has already failed here (RE §8).

## What the objective asks for

| "Both players see…" | Stage | Engine entry points found | Confidence |
|---|---|---|---|
| The same NPC walking | 1 | Movement replay on real NPCs | Proven on proxies only |
| The NPC draw a weapon | 1, 2 | Drawn state read directly. `iSyncWeaponDrawState` | Read VERIFIED, replay untested |
| The NPC get injured, die | 1 | `DamageValue`, `KillSilent` | VERIFIED statically, untested |
| The NPC stagger, fall, get up | 3 | Graph handlers: `GetUpStartHandler`, `GetUpEndHandler`, `KnockdownHandler`, `RagdollStartHandler`, `StaggeredStopHandler`, class `StaggerMeter` | Classes VERIFIED (RTTI), behaviour UNKNOWN |
| The same encounter | 2, 3 | `TESCombatEvent` sink exists. `GetCombatState` read | Partial |
| The NPC change clothes | 4 | `TESEquipEvent` (source ID 64135), `ActorEquipManager` Equip/Unequip (IDs 101949/101951) | IDs from CommonLibSF, unverified on this runtime |
| The same face and body | 4 | `TESNPC::CopyAppearance` field list (RE §9) | Partial |
| Facial expressions | 5 | `BSFaceGenAnimationData`, `BGSFaceFX`, `FaceEmotionalIdleHandler`, `ClearExpressionOverride` `0x1FC5660` | Partial |
| Head tracking, eye tracking | 5 | `HeadTrackingOnHandler`/`Off`, `HeadTrackingRotationOnHandler`/`Off`, `EyeTrackingOnHandler`/`Off`, `LookAtChainData`, `ClearLookAt` `0x1FC56F0` | Classes VERIFIED, behaviour UNKNOWN |
| Weapon transitions, blending | 2, 5 | Graph input events, as proven on proxies | Proven on proxies only |
| The same conversation | 6 | `MenuTopicManager` (ID 938421), `DialogueMenu_OnDialogueSelect`, `TESTopicInfoEvent`, `ObjectReference.Say` `0x207EF20` | Partial |
| Scenes | 6 | `SceneManager`, `BGSScene` plus 12 action classes, `TESSceneEvent`, `Scene.Start` `0x1FFD320` | Partial |
| Quest consequences | 7 | `TESQuestStageEvent`, `Quest.IsStageDone` `0x20CB530`, `BGSStoryManager` | Partial |
| Interaction events | 8 | `TESActivateEvent`, `TESFurnitureEvent`, `TESOpenCloseEvent`, `TESContainerChangedEvent` sinks | Sinks exist, sources unresolved |

## Stages

### Stage 0 — Foundation. Done

Authority, protocol 1.6, snapshots, damage and death routing, client takeover and release.

### Stage 1 — Prove the takeover on real NPCs. **Current**

Run tests 1 to 13 of the status doc.

- **Gate:** tests 1 to 8 pass with no crash, and NPCs hold up for 10 minutes in a busy location.
- **If NPC AI fights the takeover:** suppress package evaluation for taken-over actors (RE 12.2). If that fails too, the whole client model needs rethinking, for example spawning separate stand-ins. This is the largest risk in the project.

### Stage 2 — NPC actions

Capture graph input events on the authority's NPCs and replay them, the same way as player proxies: attacks, reloads, grenades, weapon switches, cover, sneak.

- Needs a hook on the Actor holder's `NotifyAnimationGraph`, which runs for every actor, so the filter must be cheap and allocation-free.
- Also stops the client's local AI from firing shots the authority did not. This is where local combat decisions must be resolved, including who simulates a shot at which player.
- **Gate:** a firefight looks the same on both screens, and shots and reloads match.

### Stage 3 — Physical states

Hit reactions, staggers, knockdowns, ragdoll and getting up, bleedout, and recovery. Ragdolls are physics, so they will diverge between machines: the outcome and final position must match, not each frame.

- **Gate:** a downed NPC lands in the same place on both screens and gets up at the same time.

### Stage 4 — Identity and appearance

- Check that a reference is the same person on both machines: base, template, appearance fingerprint. Refuse or correct mismatches.
- Equipment slots, weapons and mods, and dynamic changes through `TESEquipEvent`.
- Runtime `FF…` NPCs: an announce-and-spawn protocol with a stable network ID, plus duplicate prevention and interest management. These may be most encounter NPCs. Measure that first.
- **Gate:** generated, leveled and unique NPCs look identical, and clothing changes appear on both screens.

### Stage 5 — The small details

Head tracking and look targets, eye tracking, expressions, idle chatter, furniture and sitting, workstations, turning-in-place, and aim. Aim has an unsolved blocker of its own (A.13), shared with player proxies.

- Investigate which of these the engine drives from graph events, which come from AI, and which can be derived locally. Do not send what the client can compute.
- **Gate:** side-by-side video of one NPC doing a daily routine shows no visible difference in these details.

### Stage 6 — Dialogue and scenes

Open questions, in order:

1. Can a second player observe a conversation that is running on another machine, or does the engine tie it to the local player? Scene and topic code suggests the local player is assumed in many places.
2. Does running the same topic with `Say` on the client give matching voice, lip sync and facial animation?
3. How are the host's choices sent, and who is allowed to choose? A shared conversation controller may be needed.
4. Interruptions, cancellation, and joining mid-conversation.

Dialogue conditions are evaluated per machine, so they can disagree. Scenes refuse to start unless their parent quest is running (RE 12.4), which ties this stage to Stage 7.

- **Gate:** one player talks and the other watches and hears the same conversation, including a choice and an interruption.

### Stage 7 — Quests

The hardest stage. **Blindly syncing every quest stage is wrong**, and Starfield's quests can gate on individual progress.

- Build an inventory of how quests actually use stages, aliases and scripts.
- Classify progress as shared world, per-player or conflicting. Decide what happens when two players' saves disagree, which is the normal case for a session between two real players.
- Prevent double execution of quest scripts on every machine. This also affects deaths and activations from Stage 1 on.
- Protect essential and major characters. A quest NPC must never end up dead or missing on one machine only.
- **Gate:** one main quest and one side quest are completed together with no script running twice, and a late joiner sees the right state.

### Stage 8 — World

Doors, elevators, containers, dropped items, physics objects, hazards, traps, and NPCs crossing cells. NPCs near a client but not near the authority also need an answer here, probably per-location authority.

- **Gate:** two players split up and regroup across cells with no duplicated, vanished or stuck NPCs.

## Cross-cutting research

| Topic | Why | First step |
|---|---|---|
| Event sources | Listening beats polling and hooking. Most sources are unresolved (RE 12.6) | Count-only runtime probe to validate candidates |
| Different saves | Everything so far assumes both players load one save. Real co-op will not | Decide the session model before Stage 7 |
| Performance | Per-frame cost with dozens of NPCs is unmeasured | Measure in Stage 1 |
| Game updates | Every offset here is for 1.16.244 | Keep evidence reproducible with `tools/re/` |
