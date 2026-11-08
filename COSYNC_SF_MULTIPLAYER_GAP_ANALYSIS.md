# CoSync-SF: Multiplayer Gap Analysis

Game: Starfield 1.16.244 (`Starfield.exe` SHA-256 `7e9adb14...0f9d28`), SFSE 0.2.21, protocol 1.18. Written 2026-11-08 from the repository, the running game, the executable and the PULSAR source.

Evidence for addresses and offsets is in [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md). The newest engine findings are in [section 8](#8-engine-findings-from-this-pass). Nothing below was marked as working because it compiles.

## 1. How to read this

| Status | Meaning here |
|---|---|
| VERIFIED | Ran in a live session of two game copies on one PC, and the receiving copy's engine read the result back. No item has been played across two machines or with real network latency |
| IMPLEMENTED_UNVERIFIED | Code exists and has not been shown working in game |
| PARTIAL | Some of it works in game, and the gap is named |
| HOOKED_ONLY | An engine event or hook exists, with no finished multiplayer feature on top |
| MISSING | No meaningful implementation |
| BLOCKED | Needs named reverse engineering or a technical breakthrough |
| DECISION_REQUIRED | Needs a design decision from the project owner. The options are in [section 7](#7-decisions-needed) |

Priority: **P1** core reliability, **P2** complete world, **P3** exploration, **P4** progression, **P5** polish.

Table columns: *Existing* names the source files. *Engine* names known functions and the reverse engineering still needed (RE). *Test → result* is the procedure and what passing looks like. "Two-copy test" means `scripts/launch-instances.ps1 -AutoLoad <save> -TestCommands ...`, with the result read from the receiving copy's log.

## 2. What matters most

The existing work is broad and honest about its evidence. Seven gaps carry most of the risk:

1. **Every test so far happened in one place.** The world, NPC and proxy results all come from the `CoSyncTest` save in New Atlantis, plus one ship save. No test has run on a procedurally generated planet surface. PULSAR's notes describe a crash there (a clone's `parentCell` pointing at a cell the game had already destroyed). CoSync moves proxies through the same engine paths, so this risk applies until tested.
2. **Combat is only partly shared.** A hit on a stand-in now reaches its player when `PlayerHits` is on, and a death returns the player to a checkpoint, but who fired is not known, and each copy still runs its own NPC combat decisions. See [section 3C](#c-combat-and-damage).
3. **Two people share one character.** The model is "every copy runs the same save", so a guest has the host's face, level, perks, credits and home ship. There is no profile that belongs to a player. This decides identity, progression, inventory and ships.
4. **There is no way to get a guest into the host's world state.** The session assumes both players already hold an identical save. A real friend would have a different save, and nothing transfers one or checks that they match.
5. **Long-lived quest state is not synced.** Stages sync. Papyrus script variables, global variables and alias fills do not, and quest scripts run on every copy.
6. **Travel has no rule.** A grav jump, fast travel or landing by one player leaves the others behind.
7. **Issue #23 (actor update job freezes the guest after minutes)** is still open and undermines any long session.

## 3. Gap matrix

### A. Player identity and character

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| Stand-in actor | VERIFIED | `Avatars.cpp`, `GameBridge::UpdateAvatar`: `PlaceActorAtMe`, validated with engine probes | Survives a planet cell change: untested | `0x207A220`, `0x5A0520` (RE 2, 3) | | Add a planet soak | Two-copy test on a planet surface, walk 2 km → no crash, proxy stays valid | P1 |
| Face, body, skin, hair | BLOCKED | Proxy uses base `00000007`, so it shows the *local* player's look. Identical only because both copies load one save | The remote player's real appearance | `TESNPC::CopyAppearance` `0xCD8DF0`; bulk copy `0xCD56E0` not decompiled. The copy writes to the **base** record, shared by all its actors (RE 9) | A proxy-owned base record; player profile | Ship a plugin record or clone a base for each remote player, send the fields `0xCD8DF0` copies, rebuild 3D as `0x17A96D0` does | Different characters on two copies → each sees the other's face | P1 |
| Armor, apparel, helmet, spacesuit, backpack | VERIFIED | `GameBridge::PublishLocalOutfit`, `ApplyOutfit`, protocol 1.18 `PlayerOutfit` (section 9) | Armor mods, helmet hide settings, long-run graph safety | `ForEachEquippedItem`; Papyrus `EquipItem`; `ActorEquipManager::EquipObject` ID 101949 (`0x19A20C0`) | | Add instance data for mods | `@unequip` on one copy → the other's proxy loses it, read back from its engine | P1 |
| Equipped weapon, drawn state | VERIFIED | Transform tag 3, `ApplyWeapon` | Mods: the base form is sent, not the instance | Mods live in instance data, so the Papyrus path cannot carry them | Instance-data RE | Send the attached mod list and apply through `ActorEquipManager` with a `BGSObjectInstance` | Modded rifle on one copy → same model and scope on the other | P2 |
| Level, health for UI | VERIFIED | Transform tag 4 | | `GetValuePercentage`, `GetLevel` | | | Existing | P1 |
| Oxygen, CO2, status effects, buffs | MISSING | | All of it | Actor values, active effects | | Send a compact status bitset; show on the group panel only. Effects stay per player | Inhale O2 on one copy → panel shows it | P5 |
| Damage resistance | MISSING | | The proxy has the local base's stats, so hits on it are computed wrongly | Actor values | Combat | Send resistances, or compute damage on the victim's copy | Armored player takes less on both | P2 |
| Skills, perks | DECISION_REQUIRED | Shared through the save | Per-player values | Perk natives; PULSAR's profile design is a lead | Profile | See D3 | | P4 |
| Player death | VERIFIED | `PlayerRespawn.cpp`: no death in a session reloads a save, the player returns to a checkpoint (section 9) | Planets, ships, space. Returning into a fight still going on | Kill `0x1906010`, player death slots 386 and 367, RE 12.13 | | Add a penalty or safer return if wanted | Section 9 | P1 |
| Downed and revive | MISSING | Checkpoint return is the current rule (owner's choice, 2026-11-08) | Optional later | PULSAR's notes: the player reverts out of bleedout every frame and is never flagged dead, so it built a custom state. Lead, unverified here | Death | See D1 | | P2 |
| Name and identity | VERIFIED | Name tags, group panel, resume token | Steam identity unused for authentication | | | | Existing | P5 |

### B. Player animation

Replay through the engine's own movement path (`MoveControls.cpp`, ANIMATION_REVERSE_ENGINEERING.md A.10) is the strongest part of the project and should not be replaced.

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| Idle, walk, run, turn | VERIFIED | Controls replay, closed-loop correction | Body facing in third person (A.13) | State 1, `IMovementPlayerControls` | | | Existing | P1 |
| Jump, crouch, melee, block, fire | VERIFIED | Graph event replay, allow-list in `GameBridge.cpp` | Timing under real latency | A.11, A.12 | | | Existing, one PC | P1 |
| Aim pitch | PARTIAL | `fCameraPitch` write, `SetAim` | Confirmed correct on screen | A.13 | | Compare poses in screenshots | | P2 |
| Sprint, slow walk | IMPLEMENTED_UNVERIFIED | | Sprint is not in the controls data | `iSyncSprintState` seen on the player | | Add the sprint state to the transform | Sprint on one copy → sprint cycle on the other | P2 |
| Reload, weapon switch while moving | MISSING | Equip/Unequip left out of the replay list | Reload event replay | Graph events to name in a census | | Add once observed | Reload → proxy reloads | P2 |
| Falling, landing | IMPLEMENTED_UNVERIFIED | Jump path only | Long falls | `iSyncJumpState` | | | Fall from height → falling pose, landing | P2 |
| Boost pack (jetpack) | MISSING | | All | Not investigated | | Census of graph events while boosting | Boost → proxy rises and animates | P2 |
| Zero-g and low gravity | MISSING | | Proxy gravity follows the local copy | PULSAR warns of gravity left off after transitions | Planets, ships | Read the player's gravity scale and apply | | P3 |
| Swim, climb, mantle | MISSING | | Not checked whether the game has them. No swimming or climbing is known in Starfield | | | Confirm with graph inventory, then close | | P5 |
| Sitting, sleeping, furniture | MISSING | | Proxy never sits. NPC seating works | Furniture events 64179/64180 | | Replay the furniture activation on the proxy | Sit on one copy → proxy seated | P2 |
| Interact, use terminals | MISSING | | | Activation event | | Replay as a graph idle where one exists | | P5 |
| Stagger, hit reaction, ragdoll, get up | MISSING | | | `StaggerMeter`, `RagdollGetUp` (RTTI only) | Combat | Replay on the proxy; PULSAR blocks all of these on its clone, which is the safe default | Staggered player → proxy staggers | P2 |
| Death pose | PARTIAL | `KillActor` on the proxy | Real death test | | Death | | | P1 |
| Vehicle and ship boarding animations | MISSING | | | PULSAR lists hatch animations as unsolved | Ships | | | P3 |
| Facial, lip sync | MISSING | | | `BSFaceFX`, `BSFaceGenAnimationData` (RTTI) | Dialogue | | | P5 |

### C. Combat and damage

The model that fits the architecture: one copy (the NPC authority) decides what its NPCs do and who they hit. A hit on a stand-in becomes real damage on that player's own copy. Results of the first test are in [section 9](#9-implementation-log).

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| Player damages NPC | PARTIAL | `NpcDamage` to the authority (`NpcReplication.cpp`) | Attacker identity, so no XP or crime credit; matching health on both screens not confirmed | `DamageValue` `0x206B300` | | Carry the attacker | Shoot from guest → health equal on both | P1 |
| NPC damages guest | PARTIAL | `GameBridge::ForwardProxyHits`, protocol 1.18 `PlayerHit`, off unless `PlayerHits=1` (section 9) | Who fired. Lethal hits still reach the normal game death. Replica firing is unresolved | `TESHitEvent` 64141 (layout partly read), `DamageValue` `0x206B300` | D1 | Use the hit event for attacker identity | Section 9 run → guest's engine loses what the stand-in lost | P1 |
| Player damages player | DECISION_REQUIRED | Hits made while the authority's player is firing are not forwarded | | `BGSOnPlayerFireWeaponEvent` 64200 would be a better signal | | D2 | | P2 |
| NPC versus NPC | PARTIAL | Authority's outcome replicates | Known flaw: on a replica, NPC-on-NPC damage is forwarded as if a player did it | | | Filter by attacker | Two NPCs fight → one result | P1 |
| Shared aggro, target choice | PARTIAL | `NpcCombat` relay for data-file NPCs only | Runtime NPCs, and the fact that a replica's local AI still chooses targets | Combat controller `Actor+0x250`; `GetCombatTarget` | Hit attribution | Publish the authority's target and force it on replicas | Same enemy targets same player on both | P1 |
| Replica NPC firing | PARTIAL | Graph events replayed | In the measured window the guest took only forwarded damage, but one window is not proof. PULSAR left this undone | Projectile path not investigated | | Make replica shots cosmetic | Enemy shoots; damage counted once | P1 |
| Projectiles, impacts, explosions, grenades, mines | MISSING | | All | Not investigated: `Projectile` class family | | Visual replay only, authority decides damage | | P2 |
| Melee, criticals, stagger, knockback | MISSING | | Damage arrives as finished health loss | `CriticalHitEvent` in CommonLibSF | | | | P2 |
| Environmental, fall, hazard damage | MISSING (local by design) | Each copy's own | Must not be doubled through proxies | | | Attribute by attacker: no attacker means no forwarding | Fall on guest → hurts guest only | P2 |
| Kill attribution, XP | DECISION_REQUIRED | | | XP award `0x195FA70` (ID 101322), `RewardPlayerXP` native | Attacker identity | D4. PULSAR shares kill XP with this function | | P2 |
| Combat music, barks, alerts | MISSING (local) | Runs from each copy's AI | Divergence between copies | | | Accept for now | | P5 |
| Issue found: `@hostileproxy` chose NPCs at least 150 m away | Fixed | `WorldSync.cpp` | The earlier "guard did not move" result (issue #15) was not evidence about combat | | | Now 4 m | | P1 |

### D. NPC intelligence and behavior

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| Position, health, death, outfits | VERIFIED | `NpcReplication.cpp`, `NpcSync.cpp` | | Controller hold, RE 12.3 | | | 16-minute soak | P1 |
| Control handover | PARTIAL | Hold plus 10 s minimum | Freeze, issue #23 | Handover races the engine's own update | | Apply handover inside the actor's movement update | 60-minute soak → no freeze | P1 |
| Crowd | PARTIAL | `CrowdSync.cpp`: 70 to 84 seated on both, 18 to 21 on one | Cost of inhibition off unmeasured (#4); targets with no match (#3) | `0x127EBA0`, `0x1276A70` | | Measure, then close | | P2 |
| AI packages, schedules, patrols | PARTIAL | Follow the authority's result | NPCs near a guest but far from the authority have no owner | `EvaluatePackage` `0x1FC5E70`, `0x1914750` | Presence | Per-area authority: nearest player owns, handed over on leave | Guest alone in a cell → its NPCs behave, and host sees them on return | P2 |
| Sneaking, unconscious | HOOKED_ONLY | Flags published, not applied | Apply | `IsSneaking` | | Apply flags | | P2 |
| Sitting, working, sleeping | PARTIAL | Released to local AI when seated | Divergence after release | | | Follow furniture events | | P2 |
| Detection, stealth, investigate | MISSING | | Each copy detects only its local player | | Aggro | Authority's proxies must be detectable | | P2 |
| Taking cover, weapon choice, reload | PARTIAL | Graph events | | | Combat | | | P2 |
| Corpse persistence, respawn | IMPLEMENTED_UNVERIFIED | Death replay, `Resurrect` not handled | Cell respawn timers per copy | `iHoursToRespawnCell` | Save rules | Authority reports resets | Reset a cell → same NPCs alive on both | P3 |
| NPC changes cell, unloads, respawns | PARTIAL | Release after 5 s | Persistent NPC met elsewhere | | | | | P2 |
| Generated NPC identity | PARTIAL | Template placement, per-copy pairing by position and look | Stable across reconnect and save | FF form allocator is per copy | Entity ID | Session-wide entity IDs with base and template | | P2 |
| Vendors, unique NPC state | MISSING | | Vendor stock is per copy | Vendor container | Item ledger | Authority's vendor | | P4 |

### E. Companions and crew

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| Host's companion same on all copies | VERIFIED | Same reference, tested with Sarah (`00005986`) | Combat, dialogue | | | | Existing | P4 |
| Recruit, dismiss, assign crew | MISSING | | | `SetPlayerTeammate` natives; quest driven | Quest scripts | Host-only actions replicated as quest stages | | P4 |
| Affinity, relationship, romance | MISSING | | Per-copy globals and actor values | Globals not synced | Global sync | D3 | | P4 |
| Guest's own companion | DECISION_REQUIRED | | | | | D5 | | P4 |
| Inventory, equipment of companion | PARTIAL | Outfits sync; inventory by data-file reference | | | | | | P4 |
| Death and recovery | MISSING | | | Essential handling, untested | | | | P4 |

### F. Quests, dialogue, story

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| Stage sync, host start | VERIFIED | `WorldSync.cpp`, events 64160/64161 | | `0xD0DBD0` ID 68530 | | | Existing | P2 |
| Objectives | PARTIAL | Follows stages | Objective state set by script only | `+0x150`, state at `+40` | | Sync `SetObjectiveDisplayed` results | | P2 |
| Aliases | MISSING | | Each copy fills its own; can pick different NPCs | `AliasChangeEvent` (RTTI) | | Authority publishes fills | Radiant quest → same target on both | P2 |
| Script variables, globals | MISSING | | **Hidden dependency**: much quest logic lives in `GlobalVariable` values and Papyrus properties | Globals are forms, so they can be read and set; script variables are not reachable generally | | Sync globals through the ledger; audit quests for script-only state | Change a global → same on the other | P2 |
| Reward double payout | MISSING | Known risk in WORLD_SYNC | Fragments run on every copy | | D6 | Rewards are a per-player decision | Complete a rewarding stage → expected credits on each | P2 |
| Guest-started quests | DECISION_REQUIRED | Stay local | | | | D7, issue #9 | | P2 |
| Dialogue text to others | IMPLEMENTED_UNVERIFIED | `DialogueSync.cpp`, protocol 1.14 | A real conversation has not been tested; choices, voice | `ShowSubtitleEvent` ID 86874 | | Person-run test | Talk on one copy → text on the other | P2 |
| Two players, one NPC | MISSING | | Conflict rule | `MenuTopicManager` | | NPC busy with the first speaker | Second player refused cleanly | P2 |
| Dialogue choice shared | DECISION_REQUIRED | | | | | D8 | | P2 |
| Scenes, cinematics | MISSING | | Run per copy | `SceneManager`, `0x1FFD320` | | Authority starts, replicas told | | P2 |
| Quest failure, radiant, mission board | MISSING | Stage path only | Story manager rolls per copy | `BGSStoryManager*` tree; singleton unresolved | | Suppress story events on guests, as with crowd spawns | | P2 |
| Player-specific conditions | MISSING | | Run against the local player | | Profile | | | P4 |

### G. Inventory, economy, items

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| World pickup, first taker wins | VERIFIED | `world::Pickups`, protocol 1.15 | A person's pickup, checked against the script caveat | Event 64118 | | | Existing | P1 |
| Dropped items | VERIFIED | Protocol 1.16 | Drop in another location | | | | Existing | P2 |
| Data-file containers | VERIFIED | Protocol 1.17 stock check | Restock read as stale | | | | Existing | P2 |
| Runtime containers | BLOCKED | Refused at both ends | Runtime identity | FF forms per copy | Entity ID | | | P2 |
| Runtime NPC loot | VERIFIED | Protocol 1.16 | | | | | Existing | P2 |
| Corpse loot rolled per copy | MISSING | | Leveled rolls differ | Same generator as crowd, RE 12.10 | | Authority's inventory is the truth for corpses | Loot corpse → same items on both | P2 |
| Own inventory, credits | PARTIAL | Each copy's | Starts as a copy of the host's save | Profile | D3 | | | P4 |
| Trading between players | MISSING | | | Item ledger | | Server-arbitrated transfer | | P5 |
| Ammunition, consumables | PARTIAL (local) | Per copy | Cosmetic mismatches | | | | | P5 |
| Vendors, double purchase | MISSING | | | | | Authority's stock | Two buyers, one item → one buyer | P4 |
| Crafting, research, workbenches | MISSING | | | Script API only | D3 | Results local; shared research a decision | | P4 |
| Contraband, stolen flag | MISSING | | | `ContrabandScanWarningEvent` | Crime | | | P4 |

### H. World interaction and persistence

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| Doors, locks | VERIFIED | `WorldSync.cpp` | Doors whose cell is unloaded retry | `0x5CDC70`, 64153, 64148 | | | Existing | P2 |
| Destructibles | HOOKED_ONLY | ID 64125 sinked, never fired | A breakable object in a test | `0xB97470` | | Find one with `@destructibles` | Break → broken on the other | P2 |
| Elevators, airlocks, switches, terminals, power | MISSING | | | Activation event 63133; door types cover some | | Generalize the door path; terminal state is script | | P2 |
| Physics objects | MISSING | | | Havok not investigated | Authority model | Only authority simulates; send pose for those a player moves | Push a crate → same place | P3 |
| Traps, mines, turrets | MISSING | Turrets are actors, so partly through NPC path | | | Combat | | | P3 |
| World events, random encounters | MISSING | | Story manager per copy | | | With radiant quests | | P2 |
| Runtime reference identity | PARTIAL | Pairing for NPCs, shared IDs for drops | One general mechanism | | | Session entity table | | P2 |
| Proxies written into saves | IMPLEMENTED_UNVERIFIED | `abDeleteWhenAble` | Test | | | Save during a session, reload alone | No orphan stand-in | P1 |

### I. Worldspaces, cells and exploration

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| Stable location IDs | VERIFIED | `GameState.cpp`, bit 31 outdoors | Procedural cell IDs are FF per copy, so bit-31 distance logic may be all that matches | | | Key by planet and position | Two copies on a planet → same place recognized | P1 |
| Presence | VERIFIED | `Presence.h` for NPC traffic | World events, combat | | | Extend | | P1 |
| Procedural planets | PARTIAL | `CoSyncPlanet` save on Jemison's generated terrain. Stand-ins followed through 21 generated cells on each copy with no crash (section 9) | Moved by script in 80 m steps, not walking. No fights, creatures or long soak. Other planets | PULSAR's stale `parentCell` crash did not appear | | Soak with creatures and combat | Two-copy planet run → no crash, stand-in stays valid | P1 |
| POI and landing zone parity | MISSING | | Seeds per copy. PULSAR's top bug is POIs that differ in space | `BGSPlanet` content tree | | Send the landing site; force the same seed | Land → same POIs near | P1 |
| Fast travel | MISSING | | Each copy alone | `PlayerFastTravel::Event` | D9 | Follow the leader, or regroup | | P3 |
| Interior and elevators | PARTIAL | Proxy despawns per cell | Flicker on load | | | | | P2 |
| Loading screens | PARTIAL | VM freeze handling, menu pause hook | Stale proxies; time during a load | | | | | P1 |
| Separated players | PARTIAL | Presence filters | Authority for places only a guest is in | | | Per-area authority | | P2 |

### J. Ships

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| Own ship position reaches others | VERIFIED | `ShipSync.cpp` | | | | | Existing | P3 |
| Two players inside one ship | VERIFIED | Proxy in the interior | Flight with both aboard | | | | Existing | P3 |
| Stand-in ship | BLOCKED | | Player must be at the controls | `PlaceShipAtMe` `0x207A8B0`, space path `0x1F56A10` | Person-run test | SHIP_SYNC.md | Fly while another watches | P3 |
| Passenger model | DECISION_REQUIRED | Shared home ship | One ship or one each | | | D10. PULSAR chose one ship with a host pilot | | P3 |
| Take-off, landing, jump events | HOOKED_ONLY | CommonLibSF has real IDs; not sinked | Follow logic | `TakeOffEvent`, `LandingEvent`, `GravJumpEvent`, `FarTravelEvent` | D9 | Sink, then leader-follow | | P3 |
| Docking | MISSING | | | `DockEvent`, `InstantDock` | | | | P3 |
| Ship combat, shields, hull, weapons | MISSING | | | `Spaceship` events | Stand-in ship | | | P3 |
| Cargo, modules, builder | MISSING | | | | Ledger | | | P4 |
| Hatches, ramps | MISSING | | | `RampDownEvent` | | | | P3 |

### K. Ground vehicles

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| All vehicle systems | MISSING | | Everything | PULSAR has 1,990 lines and still crashes on a shared ride | Planets | Start with a remote player driving, with no passengers | One drives → other sees the vehicle move | P3 |

### L. Planetary exploration

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| Gravity, hazards, oxygen | MISSING (local) | Per copy | Display only | | Planets | Local is right; show on panel | | P3 |
| Survey, scan progress | MISSING | | Shared or personal | `PlanetScanEvent` | D3 | | | P4 |
| Flora, fauna, creatures | PARTIAL | NPC path covers creatures in principle | Not run outdoors | | Planets | Planet soak with creatures | | P2 |
| Resource extraction | MISSING | Refs may remove through pickup path | Harvest nodes | | | | | P4 |
| Map markers, discovery | MISSING | | Per copy | `AddToMap` | | Share known markers | | P5 |

### M. Outposts

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| All outpost systems | MISSING | | Everything. Objects are runtime references | Script API survey only | Entity ID, D3 | Host-only building first, placed as shared objects like drops | Build on host → object on guest | P4 |

### N. Factions, crime, relationships

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| Crime, bounty, arrest | DECISION_REQUIRED | | Per copy | `SendAssaultAlarm`, `SendStealAlarm`, `PlayerCrimeGoldEvent` | D11 | Options recorded | | P4 |
| Faction reputation, membership | MISSING | | Per copy | `ModFactionRank`, `GetFactionReaction` | D3 | | | P4 |
| Hostility | PARTIAL | `NpcCombat` | | | | | | P1 |
| Security scans | MISSING | | | `ContrabandScanWarningEvent` | | | | P4 |

### O. World simulation

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| Time of day | VERIFIED | Calendar, authority clock | Sleep and wait | | | Host's wait moves all | | P2 |
| Weather | VERIFIED | `fw` console | Different planets | | | | Existing | P3 |
| Respawn timers, resource regrowth | MISSING | | Per copy | | | Authority reports | | P3 |
| Story manager events | MISSING | | See F | | | | | P2 |

### P. Interface

| Feature | Status | Existing | Missing | Engine | Depends | Proposal | Test → result | P |
|---|---|---|---|---|---|---|---|---|
| Name tags, group panel, invites | VERIFIED | `MetroUi.cpp`, `Overlay.cpp`, `SteamInvite.cpp` | Across machines | | | | Existing | P5 |
| Join and leave notices | PARTIAL | Log only | HUD notification | `Debug.Notification` is wired | | Show on event | | P5 |
| Map markers, distance | PARTIAL | Distance on tags | Map | | | | | P5 |
| Permissions, settings | MISSING | | | | | | | P5 |
| Trade, revive UI | MISSING | | | | D1 | | | P5 |
| Proximity voice | MISSING | | PULSAR's design is a lead | | | Optional later | | P5 |

## 4. Networking and reliability

| Area | Status | Assessment |
|---|---|---|
| GameNetworkingSockets, LAN and Steam | VERIFIED on one PC | No architectural problem found. Keep it |
| Reliable versus unreliable split | VERIFIED | Sensible. Transforms and NPC states unreliable, everything else reliable |
| Authority | PARTIAL | One authority for the session is the single biggest scale limit: only what it has loaded is simulated |
| Duplicate protection | PARTIAL | Ledger and echo filter exist for world events. New events need the same care |
| Interest management | PARTIAL | NPC traffic only |
| Late join, resume | VERIFIED | Reconnect after a game crash starts from nothing and re-adds container totals (WORLD_SYNC limits) |
| Version compatibility | VERIFIED | Minor versions: a 1.17 client would see the new message type as an unknown type. Check before mixed versions are ever allowed |
| Two machines, real latency, 3+ players | BLOCKED | Needs a second PC or a latency shim. **A latency and loss simulator in `GnsTransport` would close much of this without a second PC** (GNS has fake-lag settings) |
| Stable entity identity | PARTIAL | Per-feature solutions. A session entity table is the shared next step |

## 5. Saves and persistence

| Item | Status | Note |
|---|---|---|
| Reapply shared changes after load | VERIFIED | Container counts excluded |
| Getting a joiner the host's save | MISSING | PULSAR transfers the host's `.sfs`, then restores the client's profile. Its worst bug is failing to re-apply that profile, leaving the client playing the host's character. Any design here must make that impossible |
| Player profile | MISSING | Depends on D3 |
| Server persistence across restarts | MISSING | The ledger lives in memory |
| Save collision on one PC | Known | Two test copies share one folder |
| Autosaves in a session | Unknown | Each copy autosaves. Whether a save holds stand-ins is untested |

## 6. Problems to hunt

Each is written as a test to run, and the column says whether it can run unattended.

| Problem | Cause to check | How to test | Unattended |
|---|---|---|---|
| Crash on a planet | Stale `parentCell` | Planet soak | Yes, with a surface save |
| Stand-in flicker on cell change | Respawn on every location change | Log respawn count while crossing a cell border | Yes |
| Wrong appearance | Base record | Two different saves | Needs design |
| Animation graph crash on equip | PULSAR's disabled path | Repeated `@equip`/`@unequip` during movement | Yes |
| Gravity left off | Transition | Read gravity after boarding and leaving | Yes |
| Different POIs | Seed per copy | Compare `@cell` output on both after landing | Needs a landing |
| Duplicate NPCs after respawn | Pairing | Long soak, count unmatched | Yes |
| Quest stages diverging | Script side effects | Compare `@quests` on both after a soak | Yes |
| Dialogue twice | Scene per copy | Person-run | No |
| Items duplicating on resume | Container totals | `@netdrop` tests, extended | Yes |
| Late joiner with incomplete world | Ledger limits | Join after 16,384 changes | Yes |
| Reconnected player duplicate rewards | Fragments | With D6 | Later |
| Stale entities after a load | Proxy not released | Load during session, count proxies | Yes |
| NPCs attacking different targets | Local AI | Log targets on both copies | Yes |

## 7. Decisions needed

Work continues around these. Nothing below has been decided.

| ID | Question | Options | Lead recommendation |
|---|---|---|---|
| D1 | What happens when a player would die? | **Decided 2026-11-08:** return to a checkpoint, and guests cannot save. Revive could be added later | Done, section 9 |
| D2 | Friendly fire? | Off, on, or a session setting | Off by default, a setting later |
| D3 | Whose character? | (a) Everyone plays a copy of the host's save. (b) Each player has a profile applied on top of the host's world | (b), the original goal, and the largest piece of work |
| D4 | Kill XP | Killer only, shared equally, or full to all nearby | Full to all nearby in a group |
| D5 | Guest companions | Host only, or one each | Host only until D3 lands |
| D6 | Quest rewards | Each player gets the full reward, or only the host | Each gets it, because of D3 |
| D7 | A quest a guest starts | Local only, spreads to all, or leader only | Issue #9 |
| D8 | Dialogue choice | Speaker decides, or a vote | Speaker decides |
| D9 | Travel | Leader pulls everyone, each travels alone and regroups, or prompts | Leader pulls the group, with a prompt |
| D10 | Ships | One shared ship, or each their own | One shared ship first |
| D11 | Crime | Shared bounty, personal bounty, or no consequence | Personal bounty, shared hostility from witnesses |

## 8. Engine findings from this pass

All on 1.16.244. Confidence as in REVERSE_ENGINEERING.md.

| Function | Address Library ID | RVA | Finding | Confidence |
|---|---|---|---|---|
| `ActorEquipManager::EquipObject` | 101949 | `0x19A20C0` | Nine arguments, matches CommonLibSF's declaration. Not used by CoSync | VERIFIED (decompiled, arity) |
| XP award | 101322 | `0x195FA70` | Takes an actor, XP as a float, a source code and a flag. Lead from PULSAR | Present; behaviour from PULSAR, not re-derived |
| Actor kill dispatch | 100769 | `0x1906010` | Reads the process at `+0x228` (`a1[69]`) first | Present |
| Death handler | none used | `0x1906250` | Large (2,296 lines decompiled) | Present |
| Life state setter | 100787 | `0x190BFF0` | Lead from PULSAR | Present |
| Player death reload | 102812 | `0x1A59040` | Lead from PULSAR | Present |
| Enter bleedout | none used | `0x197C840` | Lead from PULSAR | Present |
| Create character controller | 101052 | `0x1939630` | Same ID as the "other reader of the AI bit" in RE 12.2 | Present |

| Every global event | 64095 to 64237 | Getters | Named from the vtable of each static event source. Full table in REVERSE_ENGINEERING.md 12.12, reproducible with `tools/re/eventnames.py`. Includes hit, death, bleedout, combat, crew, alias, crime, survey and fire-weapon events that this matrix depends on | VERIFIED (RTTI, with controls) |
| `TESHitEvent` payload | 64141 | | Starts with the hit position as three floats. Offset +0x58 held a pointer to the weapon form in two samples (`000001F4`). Attacker and target fields not identified | Partly decoded at runtime |

The table corrects two earlier labels: 64179 and 64180 are trigger enter and leave, not furniture events, and 12.6 of the RE notes (event sources could not be resolved statically) no longer holds.

Not yet investigated: Havok, projectile creation, damage calculation, procedural cell lifetime on this build, spaceflight simulation.

### PULSAR

PULSAR ([source](https://github.com/ramisotti13-eng/pulsar-tool), GPL-3.0-or-later with section 7 attribution terms) is the closest prior work, unmaintained, two players, same game version. It was read for leads only. **No code was copied.** Copying any would carry its attribution and naming terms into CoSync's release obligations, on top of CommonLibSF's GPL.

Where it helped: the planet `parentCell` crash, the player having no usable bleedout state, the save-transfer plus profile approach and its top bug, equipment on a clone crashing the graph, and clamping combat randomness (`fCombat...Chance` settings) to reduce AI divergence. Each is recorded above as a lead and needs checking here.

## 9. Implementation log

### Armor and apparel on stand-ins

**Why first:** it is the most visible identity gap, needs no design decision, and reuses a path NPCs already proved. PULSAR disabled its version after crashes in the clone's animation graph, so it was tested directly.

**How it works:** each copy reads its player's worn armor once a second and sends the list when it changes. The server keeps the newest for late joiners and never replays a player's own. The receiver compares with what the stand-in wears in its engine, and uses `EquipItem` and `UnequipItem`, backing off on repeated failure.

**Result, two copies, 2026-11-08, `CoSyncTest` save:** copy 2 took off `001E2B17`. Copy 1 received the list, corrected the stand-in 2 s later, and its engine read back 3 items, `0001D1E7 001E2B18 001E2B19`. Putting it back returned 4 items, read back the same way. No crash in that session, but this is two changes, not a soak. The late-join path is covered by a unit test, not in game.

**Not done:** armor mods, the setting that hides a helmet in safe areas, and any test with two different saves.

### Hits on a stand-in reach the real player

**How it works:** on the NPC authority's copy only, a stand-in's health is read every 200 ms. A loss becomes a `PlayerHit` to that player as a share of maximum health, the stand-in is put back to full, and the player's own engine applies it through `DamageValue`. The server refuses reports from anyone but the authority. If the authority's own player fired in the last 1.5 s the loss is not forwarded, because friendly fire is undecided (D2). A stand-in that a hit killed is resurrected, since only its owner's copy decides its death.

**Result, same session:** a scripted 10% hit on the stand-in took copy 2's engine from 100% to 90%, and a 20% hit to 70%. After copy 2 started a fight with a data-file NPC (`@hostiledf`), other NPCs reported fighting player 2, and the hits their copy-1 versions landed on the stand-in reached copy 2. Copy 2 applied 137 reports in the whole run, including the two scripted ones and some small losses that came before that fight and whose cause is unknown. In one stretch the guest's own health tracked the forwarded amounts exactly (88.6, 86.4, 82.9, 81.7), so nothing was being added locally during it.

**Limits, stated plainly:**
- Off by default (`[Experimental] PlayerHits`, launcher `-PlayerHits`), like the other experimental sync options. A lethal hit now ends in a checkpoint return, not a reload.
- Attackers are not identified. Forwarded losses may include other causes of stand-in health loss.
- Two earlier runs showed steady health loss with fire input on the host copy that no test step issued. That input's source was not found, so those runs are not counted as combat evidence.
- One test location, one PC, no latency.

**Reproduce:**

```powershell
./scripts/launch-instances.ps1 -Avatars -AvatarMoveMode controls -NpcSync -WorldSync -PlayerHits -AutoLoad CoSyncTest -TestCommandDelay 40 `
    -TestCommands '@keepalive;@watchhealth;@worn;@damageproxy 10;@worn;@damageproxy 20' `
    -TestCommands2 '@keepalive;@watchhealth;@worn;@unequip 001E2B17;@worn;@equip 001E2B17;@worn;@hostiledf'
```

Read `[outfit]` and `[combat]` lines in `CoSyncSF-p1.log` and `CoSyncSF-p2.log`. Pass: the stand-in's `@worn` list follows copy 2's, and `[status] local health` on copy 2 falls by the amounts copy 1 sends.

### Death returns to a checkpoint, and guests cannot save

**Rule.** In a session nobody dies. The blow that would kill the player is stopped, health is restored, and the player goes back to their last safe spot with 5 seconds of protection. It applies to host and guests, because a reload on the host would rewind the world for everyone. On by default (`[Experimental] PlayerRespawn=0` turns it off), and only while a session runs.

**How.** `PlayerRespawn.cpp` hooks the kill dispatcher and two slots on the player's vtable. The first version hooked only the dispatcher and failed: lethal damage does not go through it. A checkpoint is an invisible marker moved to the player every 4 s while they are not fighting, are above 40% health, are level and are not in space. If the marker's cell is no longer loaded, the player goes to another player's stand-in, and if there is none they stay put at full health.

Copies that do not own the world have the engine's saving-disabled flag set and re-checked. The NPC authority owns the world, so it keeps saving.

**Result, two copies, final build, 2026-11-08, `CoSyncTest` save.** Life state read 0 and health 100% after every return, the session stayed connected, and neither copy reloaded.

| Death | Copy | Moved back | Read-back distance from target |
|---|---|---|---|
| Lethal damage (`DamageValue`) | Host | 25 m | 0.0 m |
| `Actor.Kill` | Host | Already there | 0.1 m |
| Lethal damage | Guest | 30 m | 0.5 m |
| `Actor.Kill` | Guest | Already there | 0.3 m |

On the build just before it, which had the same hooks, NPCs in a real fight brought the guest to 0 health four times and each was caught the same way (returns 3 to 6).

**Saving.** The engine's own gate answered "refused" on the guest and "allowed" on the host. A console `save` on the guest wrote no file, and the same command on the host did. No existing save changed in any of these runs.

**Limits.** Tested in New Atlantis only, not on a planet, aboard a ship or in space. Quick save and autosave were not run directly: both call the gate that refused, per the disassembly. A return can land back in the same fight, as returns 4 to 6 did. The marker is a runtime reference, so a host save made in a session contains one.

**Reproduce:**

```powershell
./scripts/launch-instances.ps1 -Avatars -AvatarMoveMode controls -NpcSync -WorldSync -PlayerHits -AutoLoad CoSyncTest -TestCommandDelay 40 `
    -TestCommands '@watchhealth;@cansave;@checkpointhold 1;@nudge 25;@killselfhit;@checkpointhold 0;@killself;@cansave' `
    -TestCommands2 '@watchhealth;@cansave;@checkpointhold 1;@nudge 30;@killselfhit;@checkpointhold 0;@killself;@worn'
```

Pass: `[respawn] verify` shows a distance under 1 m, health 100% and life state `0x0`, with no `VM frozen` line after the first load.

### Planet test and moves without a loading screen

**Planet save.** `CoSyncPlanet` was made from `CoSyncTest` by moving the player west past New Atlantis until the cell was generated at runtime (`FF01B000`). New Atlantis is in Jemison's planet worldspace, so its surroundings are procedural terrain.

**Planet run, two copies.** Both copies moved 80 m at a time, 24 times, across 21 generated cells each, about 2 km. No crash and no new dump file. The stand-in stayed loaded and followed: the `INCOMPLETE` lines only mean it was in the neighboring cell for a moment. Both lethal returns worked. Each copy's generated cells have different numbers, so only the outdoors bit and the distance tell the copies they are in the same place.

**Loading screens.** Measured with `@watchload` on one copy. `ObjectReference.SetPosition` and `MoveTo` on the player opened the loading screen every time, for 1.6 to 2.4 s. `Actor::SetPosition` (vtable slot 0x137) did not, including across a cell border. Checkpoint returns now use it whenever the target is already loaded in the same worldspace, and `MoveTo` only otherwise. Two copies on the planet: 190 m returns landed exactly on target with no loading screen. A 500 m move also showed no screen, but the game put the player back about 283 m along it, because the destination was not loaded yet. So the direct path is only for places already loaded, which is the check returns make.

Only moves within a loaded place avoid the screen. Going to another planet or into an interior still needs one, which is issue #22.

### Next feasible work

1. Decode the attacker in `TESHitEvent`, then replace the firing window with it.
2. Run the same tests on a planet, which needs a surface save.
3. A latency and loss simulator through GNS settings.

## 10. Roadmap

**Agreed order, 2026-11-08.** The planet test comes first, then the six items after it, then mod support.

1. Planet testing and stale-cell safety.
2. The guest freeze, issue #23.
3. Identifying who fired, from `TESHitEvent`.
4. One NPC fight: the authority's targets forced on the other copies, their shots cosmetic.
5. A latency and packet loss simulator.
6. Getting a guest into the host's world by transferring the save, the first step to each player keeping their own character.
7. Landing, travel and POI parity.

**Then: full mod support.** Today most messages carry raw form IDs, which assumes every copy has the same load order. Mod support needs a load-order and plugin check when a player joins, form IDs sent as plugin name plus local ID, and handling for mod-added forms, scripts and records that exist on only one copy. Location IDs already use the plugin-name form (`GameState.cpp`).

The phases below are the wider plan.

The suggested order holds, with changes where this analysis found a dependency.

**Phase 1, core reliability.** Planet soak and stale-cell safety. Armor sync. Combat: authority hits reach the real player, attribution, single counting. Death handling (D1). Issue #23. Latency simulator. Save hygiene (proxies in saves).

**Phase 2, the world.** Globals and alias sync. Rewards (D6). Story manager suppression on guests. Generalized interaction (terminals, switches). Dialogue test. Entity table. Per-area authority.

**Phase 3, exploration.** Landing site and POI parity (moved earlier than ships: it blocks everything on a planet). Travel events and leader-follow (D9). Ship stand-in. Vehicles.

**Phase 4, progression.** Player profile (D3), which unlocks companions, factions, crafting and outposts.

**Phase 5, polish.** Trade, revive UI, notifications, voice, stress tests.

Why profile sits in Phase 4 even though it is the biggest identity gap: it depends on D3, and Phase 1 to 3 are useful under "same save". If D3 goes the profile way, the proxy's base record and appearance work from section A should start alongside Phase 1.
