# Working on the host

The host is `version.dll`, the C++ program the game loads, which runs plugins and gives them their API. This page is
for changing the host itself. For writing plugins, start at [Your first plugin](getting-started.md).

## How it works

1. The game imports `version.dll` and Windows loads ours from the game folder. It forwards every real export to
   `System32\version.dll`, so the game notices nothing.
2. In the game process only, an init thread checks the exe against the build the host was measured on. On that
   build it uses the measured addresses; on another it finds the name pool, the object array and `ProcessEvent` by
   their shapes in the exe's data (`eng::Locate`). It waits for the engine, and hooks the viewport client's
   per-frame `Tick` by giving that one object a copy of its vtable. No game code is patched.
   A vectored exception handler guards each host frame: a fault in the host's own code resumes at the start of the
   frame and stops the plugin that was running, or turns the host off for the session when none was.
3. Every frame, on the game thread: input, the world (player controller, map changes), races, replays, UI, the
   registry's installs and removals, then plugins.
4. Plugins are compiled from `plugins/<id>/` into separate AngelScript modules. Each callback runs within a time
   budget; a plugin that throws, or overruns more than 3 times in a minute, is stopped and shown as stopped, and the game carries on. Plugins can
   be loaded and unloaded while the game runs; an unloaded plugin's UI goes with it.

Two rules keep the host from touching freed memory: pointers are only used in the frame they were obtained, and
anything kept longer is an `eng::Weak` (pointer plus object-array slot) checked without reading the object.

## The registry

[`registry.json`](https://github.com/AnythingGoes-ballest/ballest-plugin-manager/blob/main/registry.json) lists the plugins the game can install. Each entry is pinned to a commit of the
plugin's GitHub repo and lists the SHA-256 of every file the game downloads:

```json
{ "id": "replay-manager", "name": "Replay Manager", "description": "...", "author": "AnythingGoes",
  "repo": "AnythingGoes-ballest/ballest-replay-manager", "version": "0.1.0", "commit": "<full SHA>",
  "min_host": "0.3.0", "icon": "icon.png",
  "files": { "info.toml": "<sha256>", "main.as": "<sha256>", "icon.png": "<sha256>" } }
```

- The game reads it from `raw.githubusercontent.com` (this repo's `main` branch) and downloads each file from
  `raw.githubusercontent.com/<repo>/<commit>/<file>`, with WinHTTP on a worker thread. An entry with a `path`
  (`"path": "plugins/grind-stats"`, for a repo that holds several plugins) downloads from
  `<repo>/<commit>/<path>/<file>` instead; `files` stays relative to the plugin's folder either way.
- `"category"` and `"priority"` place a plugin in the plugin manager's lists: `enhancements`, `utilities`, `fun`
  (in that order, after the plugin manager itself), higher priority first within each; `cosmetics` keeps it out of the
  plugin browser (the plugin manager's cosmetics page lists those; a plugin with no category that needs Cosmetic Kit
  counts as one too). A plugin without them is listed after the others; only the plugin manager itself is placed by
  the host (first).
- `"added": "YYYY-MM-DD"` (optional) gives a plugin a pink NEW tag in browse (or explore, for cosmetics) for 7 days, until
  its card is opened; without it, a plugin is NEW for 7 days after the game first sees it in the registry (nothing is
  NEW on a first run).
- **Cosmetic plugins** are installed into `plugins\cosmetics\<id>` instead of `plugins\<id>`, so they don't crowd the
  plugins folder; the host loads plugins from both, and moves a cosmetic plugin it finds in `plugins\<id>` (installed
  by an older host) into `plugins\cosmetics` at start. Each is a pack on the cosmetics page: getting or removing it is
  the whole plugin.
- Every file is checked against its SHA-256 before anything is written; a mismatch fails the install and leaves the
  plugins folder untouched. Files go to a hidden `plugins\.<id>.download` folder first and are moved into place
  only once all of them are verified.
- The id `cosmetics` is refused (it is the cosmetic plugins' folder).
- Only plain names are accepted (ids, file names, `owner/repo`, `path`), so a registry entry cannot point outside the
  plugins folder. Only the plugin manager (an `essential` plugin) can install or remove, and essential plugins cannot
  be removed.
- To test a registry before publishing, put a `file:///` URL in
  `%LOCALAPPDATA%\Ballest\Saved\PluginManager\registry_url.txt` (see `tools/registry.py mirror`).

### Releasing the host

1. Bump `kHostVersion` in `src/host/plugins.hpp` (and the plugin manager's version if it changed), commit, tag
   (`v0.5.0`) and push the tag.
2. `./build.sh`, then create the GitHub release for the tag with two assets: `build/version.dll` (exactly that name:
   the game downloads it) and a zip of `version.dll` plus `plugins/` for manual installs.
3. `python tools/registry.py host v0.5.0 build/version.dll`, then commit and push `registry.json`. Every host from
   0.5.0 on offers the update: it downloads `version.dll` from the release and the bundled plugins from the tag,
   checks each against the registry's SHA-256, renames the running `version.dll` aside (a loaded DLL can be renamed
   but not replaced), puts the new one in its place, and asks for a restart; the old copy is deleted at the next
   start.

### Publishing a plugin

1. Its own repo: `info.toml`, the scripts, and optionally an `icon.png` (256x256; plugins without one get the
   default icon). A `description` and `min_host` in `[meta]` show in the browser.
2. Commit, tag it with its version (`v0.1.0`) and push the tag.
3. Here: `python tools/registry.py add ../<plugin repo> v0.1.0`, then commit and push `registry.json`. Everyone's
   browser shows the new version (as an update if they have an older one) the next time it loads the registry.

### Plugins with cooked content (release files)

A plugin can ship cooked UE content (its own materials and textures) as an IoStore container in its `assets/` folder
(host 0.27.0; see [custom-assets-recipe.md](custom-assets-recipe.md)). A container is a packed binary, so it follows the
rule for binaries: **built by the maintainers from the plugin's reviewed sources, never committed to any repo.**

1. Build it from the tagged plugin repo: `python tools/make_mod_container.py <project>.uproject <plugin>/assets`. For
   Elemental Balls, that's its `unreal/Scripts/rebuild_all.py --clean`.
2. Upload the `assets/<mod>_bNNNN-Windows.utoc`, `.ucas` and `.pak` to the plugin's GitHub release at that tag.
3. `python tools/registry.py add ../<plugin repo> v0.1.0 --release-files <the three files>`. This adds:
   ```json
   "release_files": {
     "assets/elemental_balls_b0008-Windows.utoc": {
       "url": "https://github.com/AnythingGoes-ballest/ballest-elemental-balls/releases/download/v0.1.0/elemental_balls_b0008-Windows.utoc",
       "sha256": "..."
     }
   }
   ```
   and the same for the `.ucas` and `.pak`.

The game downloads release files with the plugin's other files and checks each against its SHA-256 before anything is
written. They then go through the same staging folder and swap. Release files are refused unless all of these hold:
- the name is `assets/<plain name>.utoc|.ucas|.pak`;
- the URL is under `https://github.com/<that plugin's repo>/releases/download/<tag>/`;
- the plugin's repo is under the maintainer account (`AnythingGoes-ballest`);
- the hash has 64 hex digits.

`tools/review_guard.py registry` (run in CI whenever `registry.json` changes) applies the same rules. Binaries in repos
and diffs stay refused. When the plugin starts, the host also checks the container itself: only `/Game/Mods/<mod>/`
packages and the plugin's own shader library, or it isn't mounted.

To test before publishing: `python tools/registry.py mirror <out> <plugin repo> --release-dir <folder with the files>`.
The local copy serves them from disk through its `release_base`, which the host honours only for a `file:///` registry.

## Source map (reading order)

| File | Responsibility |
|---|---|
| `src/host/layout.hpp` | Every measured offset and address in the game binary, in one place |
| `src/host/engine.*` | Reflection: objects, names, properties, calling functions (`eng::Call`), weak references, text |
| `src/host/main.cpp` | Loading, the build check, the per-frame hook, and the frame order |
| `src/host/game.*` | World context, player controller, map changes, time, cursor, typing input mode, opening maps, the game's window |
| `src/host/input.*` | Keyboard, mouse and controllers (XInput), only while the game window has focus; the primary mouse button (swapped buttons) |
| `src/host/files.*` | Files written whole: to a `.part` file, then moved over the old one (written through, retried briefly), for downloads, icons, storage and the cosmetics' files |
| `src/host/race.*` | Whether a race is running, restarts (the ball's own counter), run id, finish, the track (name, author, author time, key), the player's input, pause, saving and loading the ball, practice runs |
| `src/host/editor.*` | The track editor: pieces, selection, placements (from the handler's SpawnActor), clicks (from the pawn's click events), rotate modes (centre, mirrored), duplicate, Tab between transform boxes, the toolbar dropdowns and key list rows plugins add |
| `src/host/hud.*` | The race HUD as parts plugins lay out: found in the race UI's trees, moved by slot, sized, off/on, blinking, part tints, plugin windows; the player's ball hidden, frozen, its camera rolled and its steering mirrored |
| `src/host/replay.*` | Replay detection, playback clock, seeking, true length, camera modes (default, follow 3D, free) |
| `src/host/ui.hpp` | The retained UI model plugins describe (footer buttons, panels, windows of views, rows and widgets) |
| `src/host/widgets.*` | Building and styling the game's own UMG widgets |
| `src/host/footer.cpp`, `windows.cpp` | Turning the UI model into live widgets, rebuilt whenever the game destroys them |
| `src/host/plugins.*` | Plugin discovery, manifests, compilation, callbacks, time budgets, loading and unloading at runtime |
| `src/host/registry.*` | registry.json, installs (download, verify, swap in, start) and removals |
| `src/host/net.*`, `json.*` | HTTPS downloads and requests and SHA-256 with what Windows ships; a small JSON reader |
| `src/host/settings.*` | `[Setting]` variables: read from the script's metadata, saved, edited, `OnSettingsChanged` |
| `src/host/storage.*` | Per-plugin saved values |
| `src/host/api.*` | The script API (all bindings in one file) |
| `src/host/cosmetics.*` | Custom balls, hats and goal explosions: the Customize page's custom sections, what the player picks (the page's handler, wrapped), and wearing them on the player's own balls |
| `src/host/packages.*` | The slot table, Cosmetic Kit's settings, loadouts, and every cosmetic known (plugins' noted as their plugin's pack) |
| `src/host/customize.*` | The Customize page's additions (with Cosmetic Kit): the local/public strip, the browser button, loadout tabs, the dock-style slot tabs, the back button moved under them |
| `src/host/browser.*` | The Classic theme's cosmetic browser window (installed cosmetics; the other themes have the window's cosmetics page) and, for now, Cosmetic Kit's settings |
| `src/host/ux.*` | Building blocks for those screens: host widgets placed by position, rounded boxes per corner, the game's font and bubble hover |
| `src/host/manager.*` | The plugin manager window (the redesigned menu: plugins installed / browse, settings and console overlays, a search per page, popups, the update banner), opened by the Plugin Manager plugin's footer button while its Theme setting is any theme but Classic (Ballest Night, the default; Ballest Light; Lucid Lime). With "Classic", only its settings, from the classic menu's cog |
| `src/host/manager_logo.inc` | The window's logo, animated (its layers `pm-logo-*`, `pm-glint`, `pm-twinkle` moved with the curves in `logo_curves.inc`, both generated by a script kept outside this repo), and the welcome: on the first open since an install or update the logo plays big in the screen's middle and flies to its corner, then the window comes in on plugins / installed; the intro plays through. The corner logo replays every 10 s while the update banner offers an update. Advanced settings' DEBUG: test the first open after an update |
| `src/host/manager_cosmetics.inc` | The window's cosmetics page, included into `manager.cpp`: the plugins page's twin for cosmetic packs (the registry's cosmetic plugins): owned / explore, pack rows that open in place, folding sections, get / remove (a whole pack: its plugin) |
| `src/host/pmui.*` | Immediate-mode drawing for that window on ux's widgets: boxes, pictures, texts and text boxes asked for by key every frame, groups that clip, hover and clicks from the pointer |
| `src/host/hoverfx.*` | The Customize page's slot hover effect (glow, smoke, thread, sparks) for other screens' side buttons, either direction |
| `src/host/leaderboard.*` | The leaderboard inside a map: its player count (Steam's entry count), a note after its title, plugin rows, and a plugin's own source shown in place of the game's list for one track (rows, ghost toggles, replay mode) |
| `src/host/nativeghost.*` | The game's own replay ghosts (BallestGhostWorldSubsystem) fed with replays a plugin hands over: a ghost session for a local map, rivals, selection, the game's replay mode |
| `src/host/splitref.*` | A plugin's split reference in the race timer's delta box for one track |
| `src/host/localrecords.*` | The game's own records of the player's never-published maps: read, cleared through the game's profile save, kept out of its records for a plugin that keeps them elsewhere |
| `src/host/pluginnet.*` | `Net::` for plugins: HTTPS to the hosts a plugin declares in its info.toml only, with size, time and rate limits |
| `src/host/menubutton.cpp` | Plugin entries in the main menu's own list (`UI::AddMenuButton`) |
| `src/host/campaigns.*` | Custom campaigns: the "custom campaigns" tab in the play page's sidebar (a clone of the game's own tab widget), the campaign list and each campaign's screen in the play page's body, and the menu's back button and Escape stepping from a campaign back to the list |
| `src/host/draw.*` | Shapes plugins draw in the world (tubes, balls, glow and glass), models made solid on the player's own never-published maps, and the camera a plugin takes |
| `src/host/steam.*` | Steam's flat API: leaderboards, their entries and attached files (UGC), workshop search |
| `src/host/ghostdata.*` | Replay JSON parsed into samples, looks and splits; positions and checkpoint orders |
| `src/host/ghosts.*` | Loading a leaderboard's runs (paged, downloaded in parallel, parsed on worker threads), player balls, crowds |
| `src/host/tracks.*` | The game's tracks and the workshop's, their titles and pictures, and opening them; the player's own never-published map files raced as a race (alone or as a playlist) and maps a plugin ships installed into the player's maps folder |
| `src/host/models.*` | Models: shapes described in text, built as Geometry Script dynamic meshes and attached to a ball or hat slot; mesh parts from model files, and their animations as a mesh per frame |
| `src/host/meshfile.*` | 3D model files (glTF 2.0 and OBJ): read into triangles per material and posed, skinned meshes included. No engine code: `tools/tests/meshfile_test.cpp` checks it offline |
| `src/host/assets.*` | Cooked content a plugin ships in `assets/`. Mounted when the plugin starts: gated, then mounted past the pak signature check for that container only, then its own shader library opened. Also maps a model's `/Game/Mods/<mod>/X` to the newest build. See [custom-assets-recipe.md](custom-assets-recipe.md) |
| `src/host/modcontainer.*` | The gate on plugin containers: reads only the `.utoc` and refuses any container serving a package outside `/Game/Mods/<mod>/` (directory index plus package ids). No engine code: `tools/tests/modcontainer_test.cpp` checks it offline |
| `src/host/testchannel.*` | Test and measurement commands, from the console and from the tools below |
| `src/proxy/` | The `version.dll` export stubs (generated from the system DLL's export table) |
| `plugins/` | The bundled plugins: Plugin Manager (the footer entry; with the Classic theme, its classic menu: console, plugin browser) and Hello World (an example) |
| `third_party/angelscript/` | AngelScript 2.38.0 (zlib license): the engine and the string, array and script builder add-ons |

## Build

Needs [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) (the x86_64 UCRT build) extracted to
`tools/llvm-mingw`, and bash (Git Bash is fine). No Visual Studio.

```bash
./build.sh            # build/version.dll (plus build/version.sym.dll with symbols, for tools/symbolize.py)
./build.sh install    # also copies version.dll and the bundled plugins into the game folder
```

`install` uses the default Steam path; set `BALLEST_GAME_DIR` to the game's `Binaries\Win64` folder if yours is
elsewhere. It replaces only the bundled plugins: ones installed from the registry, and `plugins\DISABLED`, stay.

## Testing and debugging

```bash
python tools/regression.py    # launches the game and checks everything below, pass/fail; refuses if the game is open
```

The suite covers startup, fault isolation (a plugin that throws and one that hangs), both footers (main menu and in a
map), the console, the plugin browser (and, when the plugin repos are checked out next to this one, a real remove and
install from a local copy of the registry), the Replay Manager on a simulated replay (length, pause, Space, 2x rate,
scrubbing, restart at 5x, camera dropdown), a map load, and crashes. It expects the Replay Manager to be installed.
Real replays cannot be started by script, so camera modes on a real replay are checked by hand.

### Plugin cooked content

A plugin can ship cooked UE 5.8 content in `assets/`, built with `tools/make_mod_container.py`. The full procedure and
the measured results are in [custom-assets-recipe.md](custom-assets-recipe.md). Test commands:

- `assetplugin <id>`: check and mount that plugin's `assets/` containers, as at its start.
- `assetreload <id>`: reload a plugin so it mounts a newer build. After it, send `wear ball -` and then
  `wear ball <ball id>` to rebuild the ball.

There's deliberately no command that mounts a container without the check, or from anywhere but a plugin's own
`assets/` folder.

### API tests

```bash
python tools/api_tests.py             # every script API function, in the game: which work and which are broken
python tools/api_tests.py --coverage  # (no game) every documented API has a test; also run by the docs CI
```

After a game update, this says exactly which API functions still work. `tools/api-tests` is a plugin with tests for
all of them. The runner installs it for the run only, with every other plugin turned off, then launches the game.
The plugin drives the game itself (with `Console::Run` and the host's test commands) through phases: the main menu,
the Customize page, a simulated replay, an official track before and during a race, a workshop track, the track
editor with a saved map (opened, never saved) and a test run in it. A full run takes about four minutes.

Each test checks what a plugin can see: return values, the log (`Log::Line`), and the host's test commands for the
game's state (what's on screen, whether the ball is hidden, a HUD part's opacity). The report
(`build/api-test-report.md` and `.json`) lists every documented function as passing, failing, unverified, skipped
or untested. Check messages name the function they're about, so only that one fails. The plugin settings, storage,
the plugins turned off and `window_at_start.txt` are put back afterwards.

Some functions can't be checked by script: `Plugins::OpenFolder` would open a File Explorer window, and
`Editor::NextClick` needs a real click (the editor's pick traces from the real cursor). Those are skipped and checked
by hand.

A new API function needs a test: add one to `tools/api-tests/main.as`, naming what it covers. The docs CI fails
while any documented function has none. Options: `--phases core,ui,...` runs some phases only, `--track <level>`
picks the track (default Leth Trial 01, which has checkpoints besides the finish), `--editor-map <name part>` picks
the saved map, `--keep` leaves the game open.

- Log: `%LOCALAPPDATA%\Ballest\Saved\PluginManager\host.log` (host and plugins; screenshots from the suite land here too)
- `python tools/symbolize.py` names the host's frames in the newest crash report
- Start here: `python tools/sandbox.py`, run bare, shows the sandboxed test copies (slots 1-9) and the player's own
  game, each with its host version, plugins, log errors and log rate; `<command> --help` gives each command's flags and
  examples. `start next --plugin <dir> --open 3805348161` starts a slot on the test map (race tests only there or on
  your own create-mode maps, `--map <file>.balledit`), `run <slot> -c "cmd; cmd"` sends test-channel commands and prints
  only their replies (plus `wait`, `waitstate <regex>` and `shot <name>`), `reload <slot> <dir>` reloads an edited
  plugin in a running slot, `install-player <dir>` copies a plugin into the player's game for its next start, backing
  up the copy it replaces (the game is never closed), and `log` and `stop` do what they say. `sandbox.py setup-hook`
  (opt-in, the user runs it) makes each Claude Code session start with that overview. It wraps
  `tools/test_instance.py` and `dev_session.py`, which still work alone.
- Slots start **lite** by default: the D3D11 renderer and no sound, about half the GPU memory and RAM (measured headless
  on the test map: GPU 1738 -> 913 MB, RAM in use 1717 -> 909 MB), the same physics and commands, a flatter picture (no
  Lumen or Nanite). `start --full` gives the game's full look for anything judging how it looks (materials, lighting,
  video shots); `--interactive` is always full.
- `start --headless` runs a slot with no window at all (`-RenderOffscreen`, capped at 30 fps): the sandbox, plugins,
  runs and the race clock all work. `-nullrhi` is cheaper but no run starts under it and its pictures are black
  (measured 2026-10-18).
- **Agents: `python tools/agent.py`** drives a slot through the host's agent pipe (host 0.31.0+, complete sandboxes
  only): `state` (the race, track, ball position/velocity/speed, input, camera and UI as JSON), `render [name]` (a PNG of
  the whole screen, HUD and plugin windows included, windowed or headless), `drive <x> <y> <frames> [jump]` (steering,
  x right / y forward, for that many frames, then the state; `jump` presses the jump key as the game takes it),
  `seq a || b` / `repeat <n> <cmd>` (one command a frame, answered together), `reloadmap` (the last map opened again
  the same way: a workshop map about 2.4 s, the create-mode editor map through the menu about 7 s, instead of
  restarting the slot), `editorcam [x y z [pitch yaw]]` (the track editor's camera), `tap <x> <y>` (a click at a pixel of the render picture, no window or mouse needed: the plugin manager's window,
  else a footer or plugin-window button, else the game's own button there; measured on a headless copy: menu and
  Customize buttons, back, the footer and the plugin manager's tabs), `hover <x> <y>` / `hover off` and
  `-c "cmd; cmd"` (any test commands, run together). From Python, `from agent import Slot` keeps the pipe open: a
  command is about one frame (33 ms at the headless 30 fps), `state` about 8 ms, `render` about 0.1 s (measured
  2026-10-24; through the file channel one command took about 6 s). `sandbox.py run` and `shot` use the pipe
  automatically when the slot's host has it, and a quicker file channel otherwise.
- `python tools/dev_session.py commands.txt [--map Map_Track13] [--attach]` runs test-channel commands against a fresh
  game, or with `--attach` against the one a previous `--keep` run left open
- `python tools/memread.py` (as a library) reads a running game's memory, read-only, to measure layouts
- `python tools/measure_follow.py` samples the replay camera in default and follow 3D modes during a real replay

Test channel commands (one line written to `test_command.txt` next to the log) are listed in
`src/host/testchannel.hpp`. The same commands can be typed in game: footer **plugins**, then the **>_** button (the console).

### Reading the game's own types and blueprints

Rather than probing the game to find out how something works, read it. The console command `dumptypes` writes, next
to the log in `types\`:

- `types.txt`: every class, struct and enum in memory, with each property's offset, size and exact type (including
  what an array holds) and every function's parameters;
- `Ballest.usmap`: the type mappings tools like FModel and CUE4Parse need to read the game's cooked assets. With it,
  blueprints can be dumped (their Kismet bytecode), which is how the editor's click, save and palette logic behind
  the Editor API was read. Dump it again after a game update: the game moved from Unreal 5.3 to 5.8 in
  September 2026, and a mapping file from another build misreads assets.

## The docs site

This site is built from `docs/` by `.github/workflows/docs.yml` on every push to `main` that touches the docs or
`src/host/api.cpp`. The API reference pages are generated by `tools/gen_api_docs.py` from the functions `api.cpp`
registers, with descriptions and examples from `docs/api-examples.txt`. A new API function needs an entry there, or
the docs build fails.

```bash
pip install -r requirements-docs.txt
python tools/gen_api_docs.py
mkdocs serve                  # preview at http://127.0.0.1:8000
```

