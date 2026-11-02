# CoSync-SF — Development Roadmap

Legend:
- ✅ **done**: implemented and verified by automated tests (network prototype)
- 🎮 **in-game verified**: tested inside Starfield
- 🟡 **implemented, untested**: compiles, not exercised yet
- ⛔ **blocked**: blocker noted
- ⬜ **pending**


## Milestone 1 — Networking foundation (current)

- [x] ✅ Reproducible build: CMake presets + vcpkg manifest (pinned baseline) + `scripts/build.ps1`
- [x] ✅ Wire protocol v1.0: explicit messages, bounds-checked serialization, strict UTF-8, version negotiation
- [x] ✅ Transport abstraction with GNS (UDP) and in-memory loopback backends
- [x] ✅ Session server: handshake, registration, heartbeats, resume after drop, transform relay, rate/size/pending limits
- [x] ✅ Session client: state machine, RTT, roster, auto-resume, stale-update rejection
- [x] ✅ Real end-to-end host/client test over UDP loopback (GNS)
- [x] ✅ LAN discovery protocol (tested over loopback unicast)
- [x] ✅ Network worker thread with bounded game-thread queues (tested host + client on separate threads)
- [x] ✅ `cosync-server` standalone session server and `cosync-testclient`
- [x] 🟡 SFSE plugin DLL builds; exported version data verified (Address Library v2, 1.14.70+ layout, `{0x01100F40}`); **not yet loaded in game**
- [x] 🎮 Plugin loads in Starfield 1.16.244 under SFSE 0.2.21 with the Address Library; in-game host started and its local client joined (2026-10-27).
- [x] 🟡 Two-instance launcher (`scripts/launch-instances.ps1`), windowed via StarfieldPrefs.ini. Starfield's single-instance guard (`FindWindowA("Starfield")` then `SetForegroundWindow`, then exit) is bypassed by an opt-in IAT patch (`MultiInstance.cpp`, `COSYNC_MULTI_INSTANCE=1`).
- [x] 🎮 **Two Starfield copies on one PC, connected** (2026-10-27): copy 1 hosts, copy 2 joined as player 2, and each sees the other in its roster (rtt 16–33 ms). Steam did not block the second process on the same account; no Steam emulator was needed, unlike the FO4 project.
- [x] 🎮 Unfocused copy kept running with `[General] bAlwaysActive=1` (set by the launcher); verified 2026-10-27.
- [ ] ⬜ LAN test across two physical machines (broadcast discovery and firewall behaviour)

## Milestone 2 — First in-game sync: see the other player's position (next)

Goal: two real Starfield clients show each other's position in the plugin log, then as a placeholder in the world.

1. ✅ **Licensing decision**: CommonLibSF approved (2026-10-27). The repo stays private until complete. See RESEARCH §3.
2. ✅ CommonLibSF integrated via CMake (`cmake/CommonLibSF.cmake`, pinned commits, mirrors upstream xmake flags). The Address Library file name (`versionlib-1-16-244-0.bin`) is checked at load. 🟡 The DB itself is not yet installed.
3. 🎮 Local player position/angle plus parent cell read on the main thread at 20 Hz and sent (`GameState.cpp`, `GameBridge::SampleLocalPlayer`). **Verified in game 2026-10-27.**
4. 🎮 Stable `locationId` via `TESDataHandler::compiledFileCollection`. Verified in game for full-master forms (`Starfield.esm:000125B6`, cell changes detected). Medium/small masters still untested.
5. 🎮 **In-game test passed (2026-10-27), two copies on one PC:**
   - Each log shows the other's live coordinates, matching that player's own reported position exactly when stationary.
   - About 19.5 updates/s each way, 32–34 ms rtt, 0 dropped events.
   - Same-location vs. elsewhere detection followed cell changes.
   - A `stale` pose was held while the other copy was on a loading screen.
6. 🎮 **Remote-player proxies: first milestone reached (2026-11-02).** Each copy spawns a proxy actor for the other player through the engine's own `PlaceActorAtMe` → `TESDataHandler::CreateReference` path. Engine-side validation logs every proxy as `VALID`: ACHR, enabled, in the player's cell, 3D loaded within 1 s. It follows the remote pose and is gone 2 s after despawn. The user confirmed visually that it renders opaque. Details: [REMOTE_PROXY_RESEARCH.md](REMOTE_PROXY_RESEARCH.md).
   - Root causes found by RE (see [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md)):
     - `FF…` template bases are rejected by the native on purpose.
     - The semi-transparent proxies were caused by `EnableAI(false)` right after spawn (A/B tested). `AvatarFreezeAI` now defaults to off.
   - `PlaceAtMe(Form, …)` was rejected ("Type mismatch for argument 1"); `PlaceActorAtMe(actorbase, …)` works.
   - Spawning waits for the Papyrus VM to thaw after a load.
   - ⛔ Freezing AI is ruled out (2026-11-02): with AI disabled the engine stops updating the proxy's 3D. ⬜ Neutralize behaviour with AI on.
   - ⬜ Exact appearance copy of the remote player (plan in REMOTE_PROXY_RESEARCH "Update 2026-11-02"). The base is now `00000007`, not a random NPC.
   - ⬜ Walking/running animation. Graph variables are calibrated and written, but the proxy still slides, so the next step is RE of NPC locomotion selection.
   - ✅ Two-copy test rig lag explained: GPU at 99% and Windows favours the foreground window. The launcher now defaults test copies to vsync and a 0.5 render scale, and the plugin opts multi-instance copies out of power throttling.

## Outstanding blockers

| Blocker | Why it matters | What is needed |
|---|---|---|
| IDA database still building | `0xCD56E0` (appearance), NPC locomotion, `0x5A0520` need decompilation | IDA Pro 9.3 now available; run `tools/re/ida/analyze_and_save.py` |
| No WinDbg/cdb | Four crash dumps from 2026-10-27 are unanalysed | Install WinDbg (Microsoft Store) |
| Proxy AI | Without `EnableAI(false)` the NPC is a live AI actor | RE an AI-neutral approach (see REMOTE_PROXY_RESEARCH "Known gaps") |
| `IMenu::LoadMovie` unresolved for 1.16.244 | Required for the Scaleform Multiplayer tab | RE (UI_FRAMEWORK §5) |

## Testing requirements

- Every engine-facing change: `./scripts/build.ps1` (62 unit/integration tests), then `./scripts/launch-instances.ps1 -Avatars`. Check both logs for `[avatar] ... VALID` within 2 s of spawn, `despawned ref ... no form (gone)`, and no new Papyrus.0.log errors.
- Visual checks need a human (or a screenshot) and should be A/B tested against a single switch (`-AvatarFreezeAI`, `-AvatarScale`).
- Still untested: proxies across a save/load (must not persist), a cell change by one player, three or more players, and two physical machines.

## Next implementation steps

1. Proxy behaviour without freezing AI (delayed freeze after fade-in, an AI-neutral package/faction, or C++ `SetPosition`). Validate opacity and stability.
2. Save/load hygiene test for proxies.
3. Phase 5 polish: animation speed/direction from the network velocity; despawn/respawn on cell transitions (already follows cells, but needs a test).
4. Install Ghidra and decompile `0x5A0520` for a direct, synchronous spawn.
5. UI foundation (UI_FRAMEWORK §5).

## Phase A — Connectivity
- [x] ✅ Host/join workflow (protocol level), LAN direct IP
- [x] ✅ Session lifecycle, identity (session-level), connection status, diagnostics logging
- [ ] 🟡 In-game lobby/connection UI. Decision: a custom Scaleform AS3 menu plus a pause-menu entry (see [UI_FRAMEWORK.md](UI_FRAMEWORK.md)). ✅ The UI core (`CoSyncSF.UI`: commands, snapshots, themes, JSON) is unit-tested. ⬜ Scaleform view (needs `IMenu::LoadMovie` RE).
- [ ] ⬜ `SteamTransport` (P2P/SDR through the game's Steam client), Steam identity verification. See RESEARCH §6 for open questions.
- [ ] ⬜ NAT traversal for non-Steam builds (signaling service + ICE). Requires infrastructure.

## Phase B — Player synchronization
- [x] 🎮 Remote player proxies: spawn/despawn lifecycle tied to PlayerJoined/Left and cell changes, with engine-state validation (2026-11-02)
- [x] ✅ Interpolation buffer (100 ms delay, 250 ms extrapolation cap, teleport/location snap, shortest-arc angles), receive-timestamped on the network thread, drop-oldest when the game thread stalls
- [ ] ⬜ Animation state replication (graph variables / events)
- [ ] ⬜ Respawn and disconnect cleanup

## Phase C — Shared gameplay
- [ ] ⬜ Health/damage events with server-side rate/range validation
- [ ] ⬜ Weapons and firing, hit validation
- [ ] ⬜ Inventory/equipment events, loot/containers ownership
- [ ] ⬜ NPC/creature authority (cell owner), death/revival

## Phase D — Shared world
- [ ] ⬜ Cell/interior transitions; players in different locations; loading-state awareness
- [ ] ⬜ Planetary exploration, landing/takeoff, ship interiors, spaceflight (separate research)
- [ ] ⬜ World-state consistency model

## Phase E — Persistence and dedicated hosting
- [ ] ⬜ Server config file, admin commands (kick/ban), access control lists
- [ ] ⬜ Persistent characters/progression store on the session server
- [ ] ⬜ Save/recovery; server restart without losing sessions
- [ ] ⬜ Performance and soak testing
- [ ] ⬜ Research: real-client-as-authority on a headless-ish box (RESEARCH §7 model 1/2 hybrid)

## Known risks
- Every Starfield patch can break SFSE, the Address Library and engine layouts. The plugin pins its runtime list.
- CommonLibSF's GPL licence affects how the plugin can be distributed.
- Using Steam networking under Starfield's AppID has not been validated, technically or under Steam's terms.
- Engine-thread safety: any engine call outside `GameBridge::Tick()` is a bug.
- FormIDs are load-order dependent; mod lists must match between players (session handshake should eventually compare plugin lists).
