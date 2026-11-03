# CoSync-SF — Multiplayer UI Framework

Recommendation, evidence and architecture for the CoSync multiplayer interface (Multiplayer tab, host/join, server browser, player list, diagnostics, settings).

**Status (2026-11-03):**
- ✅ **UI core** (`src/CoSyncSF.UI`): `UiCommand`/`UiSnapshot`/`UiModel`, themes, JSON. New commands: `InviteFriends`, `CopyJoinCode`; the snapshot carries `joinCode` and `steamAvailable`.
- ✅ **In-game Metro-style UI**, decided 2026-11-03: a custom UI in the style of MetroFramework instead of waiting for the Scaleform menu RE.
  - `Overlay.cpp`:
    - MinHook trampolines on the real DXGI `Present`/`ResizeBuffers` and D3D12 `ExecuteCommandLists`, so they still fire behind the game's Streamline swapchain wrapper;
    - the game's direct queue is captured from `ExecuteCommandLists`;
    - per-back-buffer allocators with fences, and back buffers released before the game resizes;
    - the WndProc is subclassed: **F7** toggles the UI, and while it is open the game gets no mouse/keyboard input.
  - `MetroUi.cpp`: flat panels, accent bar, uppercase tabs, tile buttons, all from the `Theme` tokens. Dear ImGui is only the drawing/input layer.
    - **MULTIPLAYER** tab: status, Host, Join (by join code), Leave.
    - **LOBBY** tab: player table (you/host badges, where, avatar state), KICK (host only), COPY JOIN CODE, INVITE FRIENDS (STEAM).
  - Threads: UI commands are queued on the render thread and executed by `GameBridge::UpdateUi` on the main thread. Host/join/leave restart the network worker (`SessionControl`); kick goes to `SessionServer::Kick` on the network thread.
  - Steam (`SteamInvite.cpp`, the game's own `steam_api64.dll` flat API resolved at runtime):
    - the invite dialog uses the connect string `+cosync_join <ip:port>`;
    - rich presence `connect` is set while hosting;
    - the join code is the host's primary LAN IPv4 plus port.
- ⬜ Not yet:
  - accepting a Steam invite automatically (needs the `GameRichPresenceJoinRequested_t` callback; until then the invitee pastes the join code);
  - Steam relay transport (Steam's `ISteamNetworkingSockets` P2P has the same API as GameNetworkingSockets);
  - a gamepad UI;
  - the Scaleform skin.
- **Untested in game** at the time of writing.

## 1. What Starfield's UI actually is (evidence)

| Fact | Source | Confidence |
|---|---|---|
| Menus are **Scaleform GFx (ActionScript 3) SWF movies**. | `Starfield.exe`: 317 `Scaleform::` strings, GFx SWF loader strings, `.swf` paths; `Data/Starfield - Interface.ba2` | VERIFIED (RE §7) |
| No Coherent GT / HTML UI runtime in the game. | 0 hits for `Coherent`, `cohtml` | VERIFIED |
| The renderer is D3D12. | `D3D12/` folder, `sl.*`/DLSS/XeSS DLLs in the game folder | VERIFIED (files) |
| SFSE 0.2.21 `SFSEMenuInterface` v2: a callback when any menu's movie is created (`IMenu*`), and a callback when `BSScaleformManager` is created. | `sfse/PluginAPI.h` | VERIFIED (header) |
| CommonLibSF exposes `UI` (singleton `0x5FD9B80`, `RegisterMenu` `0x2543AD0`, `IsMenuOpen` `0x2544EC0`), `BSScaleformManager` (singleton `0x61DE670`), `IMenu`, `GameMenuBase`, `Scaleform::GFx::Movie`/`ASMovieRootBase`/`FunctionHandler`. | Headers + Address Library | VERIFIED addresses; behaviour UNKNOWN |
| `IMenu::LoadMovie` and the IMenu input handlers are **unresolved (ID 0)** for 1.16.244 in CommonLibSF. | `IDs.h` | VERIFIED gap; needs RE before a custom menu can load its own SWF |

## 2. Options compared

| Option | Looks native in game | Works in fullscreen / with controller | Implementation cost | Patch fragility | Verdict |
|---|---|---|---|---|---|
| **A. Custom Scaleform menu** (own SWF loaded through the game's `BSScaleformManager`, registered via `UI::RegisterMenu`) | Best: same renderer, fonts, input, sounds | Yes | High: needs `LoadMovie` RE (ID unresolved), an AS3 toolchain (Animate/JPEXS + Flex SDK), and a C++↔AS3 bridge (`FunctionHandler`) | Medium: menu IDs and vtables change per patch | ✅ **Recommended for the in-game Multiplayer tab** |
| B. Inject into an existing menu (e.g. a "Multiplayer" entry in the pause menu) via `RegisterMenuMovieCreated` | Native | Yes | Medium: patch an existing movie's display list at runtime from AS3 | High: depends on Bethesda's SWF structure | ✅ Entry point only (a button that opens menu A) |
| C. Immediate-mode overlay (Dear ImGui) hooked into the D3D12 present/swapchain | Generic tool look unless heavily themed | Mouse only; fullscreen OK | Low to medium (D3D12 hook + input capture) | Low on the game side, but D3D12 hooking conflicts with overlays (Steam, DLSS-FG, ReShade) | ✅ **Diagnostics/developer overlay only** |
| D. HTML/CSS/JS (Ultralight / CEF offscreen) composited as a texture | Fully custom and modern | Needs the same D3D12 hook as C, plus input forwarding | High; adds 50–150 MB of dependencies; CEF needs its own processes | Medium | ❌ Not justified: no HTML runtime in the game, heavy dependency |
| E. External launcher/companion window (desktop app) | Outside the game | n/a | Low | None | ✅ Optional, for host setup and server browsing before launch |

**Recommendation.**
- The in-game Multiplayer tab is a **custom Scaleform AS3 menu (A)**, opened from an entry injected into the pause/main menu (B). It is the only option that looks native, works with gamepad and fullscreen, and adds no third-party runtime. The visual direction (modern sci-fi on Starfield's style) is fully achievable in AS3: panel layouts, transitions, typography from the game's own font libraries.
- **Prerequisite RE:** `IMenu::LoadMovie`, the menu-creation callback signature (`UI::RegisterMenu`), and input handling for 1.16.244.
- Until then, CoSync stays usable through the INI and logs. A **Dear ImGui diagnostics overlay (C)** may be added for developers. It does not replace the tab.

## 3. Architecture

Rendering, UI state, networking and engine operations stay separated. The UI never touches engine objects or sockets.

```
 AS3 movie (CoSyncMultiplayerMenu.swf)          ← pure view: renders a state snapshot, emits intents
        ▲ state JSON / typed values     │ intents ("HostGame", "Join{addr}", "Kick{id}", ...)
        │                               ▼
 UiBridge (C++, main thread)  ── Scaleform FunctionHandler + Movie::SetVariable / Invoke
        ▲ immutable UiSnapshot          │ UiCommand (validated value type)
        │                               ▼
 UiModel (C++, main thread)    builds snapshots from SessionStatus/Roster/diagnostics,
                               owns UI-only state (selected tab, theme, form drafts)
        ▲ NetworkWorker::Status/Roster   │ commands posted to NetworkWorker (bounded queue)
        │                               ▼
 CoSyncSF.Network (network thread)       GameBridge (main thread, the only engine caller)
```

Rules:
1. AS3 callbacks only produce `UiCommand` values. They are validated in `UiModel` and forwarded to `NetworkWorker` (session actions) or queued for `GameBridge` (game actions). **No UI callback calls engine code directly.**
2. Snapshots are immutable and versioned. The movie re-renders when the version changes, at most 10 Hz for live data (rtt, player list).
3. Theming lives in data: a JSON theme (colours, spacing, font choices, animation timings) loaded at startup and pushed to the movie. Themes can then change without rebuilding the SWF.
4. The same `UiModel`/`UiSnapshot` feeds the optional ImGui overlay and a future external launcher, so the presentation stays replaceable.

## 3a. Groups, party HUD and the game-menu launcher (2026-11-02, untested in game)

- **Layout.** Sidebar navigation with four pages: **Play** (Host / Join / nearby LAN games / session codes / Leave), **Lobby** (every player with level, health, planet or distance, kick, group invite), **Group** and **Settings** (display name, accent colour, HUD toggles). Nothing from the earlier UI was removed.
- **Game-menu launcher.** While the game's `MainMenu` or `PauseMenu` is open, a **MULTIPLAYER** tile is drawn top-right. Clicking it opens the CoSync UI. Mouse clicks on the tile never reach the game menu, and once the UI is open the game receives no input.
  - Detection uses `UI::IsMenuOpen`, RVA 0x2544EC0 (ID 130475). This was verified on 2026-11-02: every caller loads the UI singleton qword at RVA 0x5FD9B80 (ID 937580) into `rcx` and a `BSFixedString*` into `rdx`.
- **Groups** (protocol 1.5). Groups are server-authoritative, implemented in `GroupManager`:
  - create, invite (leader only), accept, decline, leave and kick (leader only);
  - a player can be in one group at a time, and a group holds at most 8;
  - if the leader leaves, the next member becomes leader;
  - a player who disconnects is removed from their group and from pending invites.
  
  Only group members share party frames and overhead markers.
- **Player status** (transform tag 4): health % (`GetValuePercentage(Game.GetHealthAV())`), `GetLevel`, `IsInSpace`, and `GetCurrentPlanet`. The planet's name comes from its `TESFullName` via the game's `__RTDynamicCast` import (ID 184372); the type descriptors for TESForm (843049) and TESFullName (843072) were checked against `.rdata`. Status is refreshed once a second.
- **Party frames.** These are drawn bottom-right, stacked above the player's own health bar (moved from bottom-left after the first in-game test, 2026-11-02). Each shows name, level, a health bar, and the member's planet, "In orbit · <planet>" / "In space", or the distance in metres when on the same planet.
- **Overhead markers.** Each shows a diamond icon, the member's name and their distance in metres. Markers appear only when the distance is comparable: the same cell, or both players outdoors on the same named planet. A cross-planet member gets no marker.
  - **Projection is approximate.** It is computed from the player's own view: eye = position + 1.65 m, heading `angle.z`, pitch `angle.x` (assumed positive = looking down), horizontal FOV 85°. It should be close in first person and drift in third person. Reading the engine camera (`NiCamera`, vtable ID 498033) is the next step, and its layout must be verified first.

## 4. Multiplayer tab: information architecture

- **Session**: Host Game (name, max players, password, port), Join Game (address, or pick from discovery), Leave.
- **Server Browser** (future-ready): LAN discovery results now; the Steam/master list later. Columns: name, players, ping, game version, password lock.
- **Players**: list with display name, player id, host badge, ping, location (same cell / elsewhere), kick (host only).
- **Status bar**: connection state (Idle / Connecting / Handshaking / In session / Reconnecting), rtt, dropped events, protocol and game version.
- **Diagnostics**: transport stats, last disconnect reason, proxy validation (`VALID`/`INCOMPLETE` per player), log tail.
- **Settings**: display name, default host settings, interpolation delay, proxy options (scale for debugging), theme.

## 5. UI core contract (implemented)

`cosync::ui` (`src/CoSyncSF.UI/include/cosync/ui/UiModel.h`):

| Piece | What it does |
|---|---|
| `ParseIntent(name, args)` | Turns a view intent into a validated `UiCommand`, or a user-facing error. Intents: `host` (name, port, maxPlayers, password), `join` (numeric address with optional port, password), `leave`, `kick` (player), `setName`, `selectTab`, `setTheme`, `refreshServers`. All limits match the wire protocol (name 1–32 bytes, server name ≤ 64, password ≤ 64, 2–64 players). |
| `UiModel` | Holds UI-only state (tab, theme, diagnostics ring of 64) plus inputs from the plugin (connection, players with proxy state, servers, versions). The snapshot `version` increases only when content changes, so the view re-renders only on a real change. |
| `UiModel::ApplyLocal` | Executes UI-only commands (tab). Everything else goes back to the plugin, which forwards session actions to `NetworkWorker` and game actions to `GameBridge`. |
| `Theme` / `ParseTheme` | Visual tokens: colours, opacity, corner radius, spacing, font scale, transition time. They load from `key = value` text (`#RRGGBB` colours), so a theme change needs no rebuild. The default theme is "Starfield Modern" (dark panels, amber accent, cyan secondary). |
| `ToJson(snapshot)` | Strict JSON for the view. **UNKNOWN:** whether Starfield's Scaleform AS3 runtime provides `JSON.parse`. If not, the bridge will pass `GFx::Value` objects built from the same snapshot. |

## 6. Next steps (UI)

1. In IDA, reverse-engineer `IMenu::LoadMovie`, `UI::RegisterMenu` and the menu-creation callback for 1.16.244, and record them in REVERSE_ENGINEERING.md.
2. Build a minimal AS3 movie that loads, shows one text field, and round-trips one intent to C++ through `ParseIntent`.
3. Wire `UiModel` into the plugin: feed it from `GameBridge::Tick` and execute commands on the main thread.
4. Add the pause-menu entry point, then build the full Multiplayer tab on the snapshot.
