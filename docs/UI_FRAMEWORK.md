# CoSync-SF — Multiplayer UI Framework

Recommendation, evidence and architecture for the CoSync multiplayer interface (Multiplayer tab, host/join, server browser, player list, diagnostics, settings). **Nothing in this document is implemented yet.** The roadmap puts the UI after transform sync (Phase 6).

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

## 4. Multiplayer tab: information architecture

- **Session**: Host Game (name, max players, password, port), Join Game (address, or pick from discovery), Leave.
- **Server Browser** (future-ready): LAN discovery results now; the Steam/master list later. Columns: name, players, ping, game version, password lock.
- **Players**: list with display name, player id, host badge, ping, location (same cell / elsewhere), kick (host only).
- **Status bar**: connection state (Idle / Connecting / Handshaking / In session / Reconnecting), rtt, dropped events, protocol and game version.
- **Diagnostics**: transport stats, last disconnect reason, proxy validation (`VALID`/`INCOMPLETE` per player), log tail.
- **Settings**: display name, default host settings, interpolation delay, proxy options (scale for debugging), theme.

## 5. Next steps (UI)

1. RE `IMenu::LoadMovie`, `UI::RegisterMenu` and the menu-creation callback for 1.16.244, and record them in REVERSE_ENGINEERING.md.
2. Build a minimal AS3 movie that loads, shows one text field, and round-trips one intent to C++.
3. Implement `UiModel`/`UiSnapshot`/`UiCommand` with unit tests, independent of Scaleform.
4. Add the pause-menu entry point.
