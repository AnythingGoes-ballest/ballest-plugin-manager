# Ballest plugin manager: design

Status: first milestone working in game (2026-09-22). Implementation map: [Working on the host](host.md). North star: a C++ host loaded through our own proxy DLL, running AngelScript plugins.
The host is the only code that touches the game. Plugins see only the host's API.
First milestone: the host plus two plugins, a mod manager and a replay manager.

## 1. Goals and non-goals

Goals
- One install that loads cleanly into Ballest (UE 5.8 Shipping since the 2026-09-15 update) at startup.
- Plugins written in AngelScript, statically checked at load, sandboxed, with typed exports between plugins.
- Every game interaction goes through one layer that encodes the safety rules we learned (section 6).
- Plugins can be enabled, disabled, reloaded, and configured in game.
- Unapproved plugins can never submit leaderboard scores (section 7). The game developer has approved the project.

Non-goals for the first milestone
- New assets through paks. Measured: the game rejects mod paks (signed content). All content is built at runtime.
- A web repository or auto-updater. Plugins are folders on disk for now.
- Multiplayer sync of plugin state.

## 2. Measured facts this design rests on

| Fact | How it was established |
|---|---|
| Engine is UE 5.8 Shipping (FileVersion ++UE5+Release-5.8-CL-56702186, PE timestamp 1949201014), D3D12. Our old 5.3.2 usmap is stale | exe version resource and PE header, 2026-09-22 |
| Game exe imports dwmapi, version, winmm, dsound, winhttp, uxtheme; none are KnownDLLs | PE import table and the KnownDLLs registry key, 2026-09-22 |
| UE4SS already occupies dwmapi.dll | file beside the exe |
| Runtime UMG widgets, spawned actors, Geometry Script meshes all work | our mods, harness screenshots |
| DrawDebug is a no-op and hand-built components do not render in Shipping | checkpoint boxes mod |
| Reading a property a class lacks crashes natively | multiple crashes |
| Post-hooks on native functions crashed later reloads | custom pieces copy/paste |
| Replay control: BallestGhostWorldSubsystem GetPlaybackTime, SeekPlayback, RestartPlayback; AC_GhostFollowCam RotationSource | replay mods |
| Leaderboard counts: SIK_UserStatsLibrary.GetLeaderboardEntryCount, handle passed as an integer | leaderboard count mod |
| Backend rejects runs for untrusted client content; a trusted-submit path exists | strings in the game binary |
| Build machine has no Visual Studio, CMake, or Ninja. The user does not want Visual Studio | vswhere and PATH, 2026-09-22 |

## 2a. Engine layout for this build (measured live, read-only, 2026-09-22)

Measured with `tools/memread.py` against the running game. RVAs are relative to the exe base.

| Item | Value |
|---|---|
| GUObjectArray | RVA 0xA39ED40. ObjObjects at +0: Objects** +0, NumElements +0x8, MaxElements +0xC (2162688), NumChunks +0x10, MaxChunks +0x14 |
| FUObjectItem | 24 bytes, object pointer at +0x8, 65536 items per chunk |
| FNamePool | RVA 0xA2D0D40, Blocks at +0x10; entry = block + offset*2; header u16: bit0 wide, length = header >> 6 |
| UObject | vtable 0x0, ObjectFlags 0x8, InternalIndex 0xC, Class 0x10, Name 0x18, Outer 0x20 |
| UStruct | Super 0x40, Children 0x48, ChildProperties 0x50, PropertiesSize 0x58 |
| FField / FProperty | Class 0x8, Owner 0x10, Next 0x18, Name 0x20, ElementSize 0x34, Offset 0x44; PropertyClass 0x70; array Inner 0x78 |
| UFunction | FunctionFlags 0xB0, NumParms 0xB4, ParmsSize 0xB6, ReturnValueOffset 0xB8, Func 0xD8 |
| ProcessEvent | RVA 0x15F22C0 |
| FName(wchar_t*) | RVA 0x141F6A0; FName::ToString RVA 0x142D4F0 |
| Per-frame hook | The live viewport client is CommonGameViewportClient; UGameViewportClient::Tick (RVA 0x2A74F20) is vtable slot 99 and is not overridden. The host swaps that one object's vtable pointer to a copy with slot 99 replaced: no code patching, no conflict with UE4SS |
| Footer (bottom bar) | WBP_Footer_C inside WBP_MainMenu_UIManager_C. Tree: SizeBox > Overlay > [Background Image, HorizontalBox]. HorizontalBox children: master volume, music, spacer overlay, Discord button, language dropdown. The plugin manager entry is inserted before Discord |

## 3. Architecture

```
Ballest-Win64-Shipping.exe
  +- version.dll  (our proxy; forwards the real exports to System32\version.dll)
       +- Host (C++)
            +- Engine core     reflection, object lookup, safe property access, calls, hooks, game-thread tick
            +- Game adapter    Ballest concepts: race, ball, checkpoints, replay, camera, editor, menus
            +- Services        events, scheduler, UI, input, settings, storage, logging, world objects
            +- Script runtime  AngelScript 2.38, one module and context set per plugin
            +- Plugin manager  discovery, manifests, dependencies, lifecycle, trust tiers, leaderboard gate
                 +- plugins/<id>/info.toml + *.as
```

### 3.1 Loader
- `version.dll` is the proxy. The game imports it, it is not a KnownDLL, its export surface is small, and it does not collide with UE4SS (dwmapi), so both can be present during development.
- `DllMain` only sets up export forwarding and starts one init thread. The init thread waits until the engine is ready (object array populated, game engine object exists), then installs the tick hook. Everything else runs on the game thread.
- Kill switch: a launch flag or a marker file beside the exe disables the host without uninstalling.

### 3.2 Engine core
- Locate GUObjectArray, the FName pool, and ProcessEvent (signature scan, validated by sanity checks).
- Iterate objects; find by path, by class, first-of.
- Property access by name that checks the class's property chain first. A missing property is an error, never a raw read.
- Call a UFunction with a parameter buffer built from its parameter properties.
- Hooks: today only the per-frame hook (a vtable swap on the viewport client). Game functions are called through
  ProcessEvent. When plugins need game events, one ProcessEvent hook dispatching by UFunction pointer covers every
  blueprint event (see D2).
- Game-thread tick: UGameViewportClient::Tick through a vtable swap (not a blueprint ReceiveTick, which differs per map
  and mode). It runs after the world tick and before rendering.
- World tracking: each UWorld gets a generation number. Every handle the host gives out carries it and becomes invalid on map change.

The source of this layer is decision D1.

### 3.3 Game adapter
Typed wrappers built only from measured classes and functions:
- Race: StartTrack, ResetTrackManager, race restart; checkpoint touched (MP_ActualCheckpoint_Strip_C).
- Ball: BP_RollingBall_C location, velocity, input.
- Replay: playback time, seek, restart, length (from Saved/Ghosts JSON), speed through seek-driven pacing.
- Camera: AC_GhostFollowCam modes; free camera (still being measured).
- Menus: pause-menu row injection (WBP_ButtonBase_C), main menu.
- Editor: selection, categories (later milestones).
- Leaderboards: read-only records and entry counts.

### 3.4 Services
- Events: the host hooks each game function once and fans out to subscribed plugins in load order.
- Scheduler: `Main()` coroutines with `yield()` and `sleep(ms)`, plus per-frame `Update(dt)`. All plugin code runs on the game thread.
- UI: host-owned UMG widgets built through reflection (panel, text, button, slider, dropdown, checkbox, text box), exposed as a small widget API. See decision D3.
- Input: key and mouse events with modifiers; binding conflicts reported.
- Settings: declared in the plugin with attributes on globals, like Openplanet; saved per plugin; rendered by the mod manager.
- Storage: a per-plugin data folder under `%LOCALAPPDATA%\Ballest\Saved\Plugins\<id>`.
- World objects: spawned actors and runtime meshes owned by the plugin, destroyed on unload or map change.
- Log: per-plugin log with levels, visible in the mod manager.

### 3.5 Script runtime
- One AngelScript engine; one module per plugin. Plugins cannot see each other's globals.
- Each plugin has its own contexts. A line callback enforces a time budget per call (default 50 ms). An overrun suspends and disables the plugin instead of freezing the game.
- Script exceptions disable the offending plugin and show in the mod manager. They never reach the game.
- Exports: a plugin publishes `shared` interfaces and functions (listed in info.toml). Dependents import them with full type checking.
- Reload: runs `OnDisabled` and `OnDestroyed`, releases the plugin's world objects and UI, discards the module, recompiles. State can be handed over through storage.
- Native value types: vec2, vec3, rotator, color. Game objects are handles that expire with the world generation.

### 3.6 Plugin manager
- As built: `plugins/<id>/info.toml` beside the exe, one folder per plugin, whether bundled or installed from the
  registry (section 3.7). The manifest below is the fuller plan; `id` comes from the folder name today, and
  dependencies, exports and permissions are not built yet.
- Manifest:

```toml
[meta]
id = "replay-manager"
name = "Replay Manager"
version = "0.1.0"
author = "AnythingGoes"
game_version = ">=1.0"

[script]
files = ["main.as", "ui.as"]
timeout = 50

[dependencies]
required = { "ui-kit" = "^0.2" }
optional = {}

[exports]
shared = ["api.as"]

[permissions]
wants = ["replay.control", "camera.control", "ui.overlay"]
```

- Load order: topological sort on dependencies. Cycles and missing dependencies disable the plugins involved, with the reason shown.
- Permissions: each service checks the caller's granted permissions. Sensitive ones (plugin management, physics changes) need an explicit grant.
- Trust tiers: section 7.

### 3.7 Registry and plugin browser (built 2026-09-23)
- `registry.json` in this repo lists installable plugins, each pinned to a full commit SHA of its own GitHub repo
  with a SHA-256 per file (`tools/registry.py add` writes entries from a tag).
- The host fetches it with WinHTTP on a worker thread (never the game thread), verifies every file before writing
  anything, stages the files in a hidden `.<id>.download` folder and swaps them in on the game thread, between
  plugin callbacks. Removal unloads the plugin (discards its module, releases its context, takes its UI off
  screen, drops its cursor request) and deletes its folder straight away.
- Plugin slots (indices) are never reused in a session: the index names the script module and owns the UI, so a
  removed plugin's slot stays, marked removed, and a reinstall gets a new slot.
- WinHTTP and CNG are loaded with LoadLibrary on first use, so the DLL still imports only kernel32, user32 and the
  C runtime.
- Only essential plugins (the plugin manager) may install or remove; ids, file names and repo names are limited to
  plain characters so no entry can reach outside the plugins folder; `Host.OpenUrl` only opens
  `https://github.com/` pages.
- Icons: a plugin's `icon.png`, loaded as a texture with `KismetRenderingLibrary.ImportFileAsTexture2D` (measured
  present in this build) and shown with `Image.SetBrushFromTexture`; registry icons are cached in
  `%LOCALAPPDATA%\Ballest\Saved\PluginManager\cache`; plugins without one get the plugin manager's default icon.

## 4. Plugin API

The API as built is documented in the [API reference](reference/api/ui.md) and registered in one place, `src/host/api.cpp`.
Planned next: game events through a ProcessEvent hook (race start, checkpoints, replay start and end), coroutines in
Main, and hot reload. Built: per-plugin storage (`Storage`) and Openplanet-style settings (section 3.8).

### 3.8 Settings and movable windows (built 2026-09-23)
- Plugins are compiled through AngelScript's script builder add-on, which keeps each global's metadata; a global
  tagged `[Setting ...]` becomes a setting (bool, int, uint, float, double, string; name, description, min/max,
  hidden). Its value after the module is built is the default; the saved value (Storage "setting.<variable>") is
  written into the variable before Main. Values at their default are not saved.
- The plugin manager edits another plugin's variables through `Settings::Set` (essential plugins only), which writes
  the script global directly between callbacks and runs the owner's `OnSettingsChanged()` before its next Update.
  Saving is throttled (a dragged slider changes a value every frame). A plugin's settings are forgotten, after
  saving, before its module is discarded on removal.
- Movable windows get an invisible button filling the window behind the content (texts and images are made
  hit-test invisible; measured, UMG's TextBlock and Image default to Visible). While it is pressed the window
  follows `WidgetLayoutLibrary.GetMousePositionOnViewport` (measured present) through its canvas slot's
  `SetPosition`; the position is saved per plugin and window on release. A stopped plugin's windows are hidden
  and its cursor request dropped, so nothing dead is left on screen.

### 3.9 Track editor (built 2026-09-23)
- `P_LevelEditorPawn_C.SKGMLEHandler` is the editor's SKGMLEHandlerComponent: `AllActors` lists the pieces,
  `GetSelection`, `CopySelection` + `Paste(at copied location, with properties)` duplicates and leaves the copies
  selected, `Deselect` + `GrabWithNotifications` selects. The handler caches the gizmo pivot, so pieces moved from
  outside are selected again.
- A palette tile spawns its piece at pawn + 150 x view forward + (0, 0, -300); new AllActors entries at that point
  within 10 frames are placements, not pastes.
- `W_Details_C.WS_Details` slot 1 is a VerticalBox (transform header, `W_Transform`, paint). Docked plugin windows
  are appended to it; UMG has no runtime insert, so they sit under paint. Paint lists one entry per selected piece
  type (the game's own; measured with no plugin touching it).
- `W_MapEditor_C` has its own `WBP_Footer`, used like the main menu's and the race UI's.
- Measured cost of the game's own work: paste about 4.5 ms and select about 3 ms a piece (535 ms for a first copy
  of 15 new piece types). `plugins::GameWork` keeps that time off the calling plugin's budget, up to 5 s a
  callback; the first release charged it, and a copy of a few pieces stopped Create Extensions at 30 ms.

## 5. First milestone plugins

Mod manager (permissions `plugins.manage`, `ui.menu`)
- Pause-menu entry "plugins" above "exit".
- Plugin list with status (loaded, disabled, error, blocked by the gate), toggle, reload, and log view.
- Settings page generated from each plugin's declared settings.
- Shows the current trust tier and whether leaderboard submission is allowed.

Replay manager (permissions `replay.control`, `camera.control`, `ui.overlay`)
- Timeline with play and pause, scrubbing, thousandths display, correct total length, restart at the end at any speed.
- Speeds .1x to 5x, Space toggles pause, cursor visible during replays.
- Camera modes: default, follow 3D, free camera.
- Acceptance test: feature parity with the two existing Lua mods.

## 6. Safety rules the host enforces (from measured failures)
- Never read or write a property without checking the class's property chain.
- Never keep a game object across a map change; handles expire with the world generation.
- One hook per game function, owned by the host. Plugins subscribe to events.
- Pre-hooks only on native functions unless a post-hook is measured safe.
- No plugin work off the game thread. No polling loops; use events and the frame tick.
- Every plugin call is budgeted and isolated. A failing plugin is disabled and the game keeps running.
- Unload is total: hooks, UI, world objects, timers, and input bindings owned by the plugin are released.

## 7. Trust tiers and the leaderboard gate
Modeled on Openplanet's signature modes.
- Official: only plugins shipped with the host. Leaderboards allowed.
- Approved: adds plugins signed by us (a hash list signed with our key, verified by the host). Leaderboards allowed.
- Practice: adds any plugin whose manifest marks it practice-only. Leaderboards blocked.
- Developer: loads anything and implies Practice. Leaderboards blocked.

Gate mechanism: the host intercepts the game's score-submission call and drops it, with an on-screen notice, whenever the tier blocks leaderboards. The exact function must be measured before implementation. Candidates are the BallestLeaderboardSubsystem submit path and the backend submit-run request. With the developer on board, an official "modded client" flag in the game would be cleaner than dropping; worth asking for.

## 8. Decisions
- D1. Reflection core: our own, no UE4SS code (2026-09-22). All layouts in `src/host/layout.hpp`.
- D2. Hooking: no detours. The per-frame hook swaps one object's vtable pointer; game functions are called
  through ProcessEvent. A ProcessEvent hook (for game events) is still to be decided when events are needed.
- D3. UI: the game's own UMG widgets through reflection (looks native, measured working). An ImGui overlay stays
  an option for developer tools.
- D4. Manifest parsing: a small built-in parser for the TOML subset info.toml uses; no dependency.
- D5. Toolchain: llvm-mingw (clang, standalone, in `tools/`), no Visual Studio.
- D6. Distribution: plugins live in their own GitHub repos; this repo's `registry.json` pins each to a commit and
  per-file SHA-256; the game installs and removes them live (2026-09-23).

## 9. Build and test
- `./build.sh [install]` builds the host with the bundled llvm-mingw and AngelScript 2.38.0.
- `python tools/regression.py` runs the whole suite against the real game through the host's test channel
  (no mouse, no focus changes, no UE4SS): startup, fault isolation, both footers, the console, the plugin browser
  (a real remove and install from a local copy of the registry), the Replay Manager on a simulated replay with
  timing checks, a map load, and crash detection. 45/45 on 2026-09-23.
- Real replays cannot be started by script yet (the menu's level setup is skipped when a map is opened directly,
  so the leaderboard never loads); replay camera modes are checked by hand on a real replay.

## 10. Milestones
- M0: proxy version.dll loads, forwards exports, logs; per-frame hook. Done.
- M1: engine core (objects, names, properties, calls), verified live. Done.
- M2: game-thread frame, map-change handling through generations and weak references. Done. (A ProcessEvent hook
  for game events is not built yet.)
- M3: AngelScript runtime: modules per plugin, Main and Update, time budget, exceptions stop the plugin. Done.
  Coroutines (yield in Main) and hot reload are not built yet.
- M4: services: UI (footer, panels, windows of widgets), input, cursor, replay, race, storage. Done. Settings not yet.
- M5: plugin manager plugin. Done (footer entry, panel with live status, console, plugin browser).
- M6: replay manager plugin, beyond the Lua mods: true length for any replay, pause, Space, .1x-5x, scrubbing,
  restart at the end, follow 3D, free camera. Done; follow 3D confirmed by hand on a real replay, the free
  camera spawns in a real replay but its controls are not yet confirmed.
- M7: trust tiers and the leaderboard gate. Not started.
- M8: registry and live install, update and remove from GitHub. Done and verified in game from GitHub.
- M10: the host updates itself from the registry's "host" entry (release asset version.dll + bundled plugins at the
  tag, all SHA-256 checked): the loaded version.dll is renamed aside (measured: Windows allows renaming a loaded
  DLL), the new one takes its name, and the old copy is deleted at the next start. Done; tested end to end against a
  local registry (banner, update, swap, cleanup and restart).
- M9: Openplanet-style settings and movable windows. Done (settings, saving, restore and reset tested in game; the
  drag itself shown working by a real mouse drag).
