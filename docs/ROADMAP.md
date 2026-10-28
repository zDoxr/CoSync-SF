# CoSync-SF — Roadmap

Legend:
- ✅ **done**: implemented and verified by automated tests (network prototype)
- 🎮 **in-game verified**: tested inside Starfield
- 🟡 **implemented, untested**: compiles, not exercised yet
- ⛔ **blocked**: blocker noted
- ⬜ **pending**

Nothing is 🎮 yet.

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
6. 🎮 Remote-player avatars, partly working (2026-10-27): `PlaceActorAtMe` spawns an actor per remote player in the same cell. It is moved to the interpolated pose via Papyrus `SetPosition`/`SetAngle` (10 Hz) and despawned when the player leaves the cell.
   - `PlaceAtMe(Form, ...)` was rejected ("Type mismatch for argument 1"); `PlaceActorAtMe(actorbase, ...)` works.
   - Spawning must wait for the Papyrus VM to thaw after a load.
   - Cloning the player's base NPC `00000007` rendered only a faint floating head (it has no body or outfit of its own). Now a nearby NPC's base is used; the player base is only a fallback. ⬜ Copy the remote player's actual appearance.
   - ⬜ Walking/running animation (the avatar currently slides).

## Phase A — Connectivity
- [x] ✅ Host/join workflow (protocol level), LAN direct IP
- [x] ✅ Session lifecycle, identity (session-level), connection status, diagnostics logging
- [ ] ⬜ In-game lobby/connection UI. Options: SFSE Menu interface (Scaleform), or a console command first.
- [ ] ⬜ `SteamTransport` (P2P/SDR through the game's Steam client), Steam identity verification. See RESEARCH §6 for open questions.
- [ ] ⬜ NAT traversal for non-Steam builds (signaling service + ICE). Requires infrastructure.

## Phase B — Player synchronization
- [ ] ⬜ Remote player proxies: spawn/despawn lifecycle tied to PlayerJoined/Left and cell loads
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
