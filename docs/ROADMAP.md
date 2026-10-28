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
- [x] 🟡 SFSE plugin DLL builds with exported version data for 1.16.244; **not yet loaded in game**
- [ ] ⛔ Load the plugin in Starfield 1.16.244 under SFSE 0.2.21. Blocked: the game was still installing on the dev machine during this pass.
- [ ] ⬜ LAN test across two physical machines (broadcast discovery and firewall behaviour)

## Milestone 2 — First in-game sync: see the other player's position (next)

Goal: two real Starfield clients show each other's position in the plugin log, then as a placeholder in the world.

1. ⬜ **Licensing decision**: link CommonLibSF (GPL-3.0+ with modding exception) or not. See RESEARCH §3.
2. ⬜ Integrate CommonLibSF (xmake vs. CMake decision) and verify the Address Library for 1.16.244.
3. ⬜ On the main thread, read `PlayerCharacter::GetSingleton()` position/angle plus parent cell/worldspace → `SubmitLocalTransform` at 20 Hz.
4. ⬜ Define a stable `locationId` mapping (plugin file + local FormID → session-scoped id table), sent reliably.
5. ⬜ In-game test: two PCs (or two accounts) connect; each log shows the other's live coordinates.
6. ⬜ Spawn a placeholder reference/actor for each remote player and move it every frame (interpolated). Needs RE: spawning API, disabling AI, and safe teardown on cell unload.

## Phase A — Connectivity
- [x] ✅ Host/join workflow (protocol level), LAN direct IP
- [x] ✅ Session lifecycle, identity (session-level), connection status, diagnostics logging
- [ ] ⬜ In-game lobby/connection UI. Options: SFSE Menu interface (Scaleform), or a console command first.
- [ ] ⬜ `SteamTransport` (P2P/SDR through the game's Steam client), Steam identity verification. See RESEARCH §6 for open questions.
- [ ] ⬜ NAT traversal for non-Steam builds (signaling service + ICE). Requires infrastructure.

## Phase B — Player synchronization
- [ ] ⬜ Remote player proxies: spawn/despawn lifecycle tied to PlayerJoined/Left and cell loads
- [ ] ⬜ Interpolation buffer (100–150 ms), extrapolation cap, and packet-loss handling (protocol side is already unreliable + sequenced)
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
