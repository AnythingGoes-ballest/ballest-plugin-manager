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
   budget; a plugin that throws or overruns is stopped and shown as stopped, and the game carries on. Plugins can
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
  `raw.githubusercontent.com/<repo>/<commit>/<file>`, with WinHTTP on a worker thread.
- Every file is checked against its SHA-256 before anything is written; a mismatch fails the install and leaves the
  plugins folder untouched. Files go to a hidden `plugins\.<id>.download` folder first and are moved into place
  only once all of them are verified.
- Only plain names are accepted (ids, file names, `owner/repo`), so a registry entry cannot point outside the
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

## Source map (reading order)

| File | Responsibility |
|---|---|
| `src/host/layout.hpp` | Every measured offset and address in the game binary, in one place |
| `src/host/engine.*` | Reflection: objects, names, properties, calling functions (`eng::Call`), weak references, text |
| `src/host/main.cpp` | Loading, the build check, the per-frame hook, and the frame order |
| `src/host/game.*` | World context, player controller, map changes, time, cursor, typing input mode, opening maps |
| `src/host/input.*` | Keyboard and mouse, only while the game window has focus |
| `src/host/race.*` | Whether a race is running, and restarts from the beginning (the ball's own counter) |
| `src/host/editor.*` | The track editor: pieces, selection, placements, duplicate, rotate about a point, Tab between transform boxes |
| `src/host/replay.*` | Replay detection, playback clock, seeking, true length, camera modes (default, follow 3D, free) |
| `src/host/ui.hpp` | The retained UI model plugins describe (footer buttons, panels, windows of views, rows and widgets) |
| `src/host/widgets.*` | Building and styling the game's own UMG widgets |
| `src/host/footer.cpp`, `windows.cpp` | Turning the UI model into live widgets, rebuilt whenever the game destroys them |
| `src/host/plugins.*` | Plugin discovery, manifests, compilation, callbacks, time budgets, loading and unloading at runtime |
| `src/host/registry.*` | registry.json, installs (download, verify, swap in, start) and removals |
| `src/host/net.*`, `json.*` | HTTPS downloads and SHA-256 with what Windows ships; a small JSON reader |
| `src/host/settings.*` | `[Setting]` variables: read from the script's metadata, saved, edited, `OnSettingsChanged` |
| `src/host/storage.*` | Per-plugin saved values |
| `src/host/api.*` | The script API (all bindings in one file) |
| `src/host/cosmetics.*` | Custom balls, hats and goal explosions: the Customize page's custom sections, what the player picks (the page's handler, wrapped), and wearing them on the player's own balls |
| `src/host/leaderboard.*` | The leaderboard inside a map: its player count (Steam's entry count) and a note after its title |
| `src/host/models.*` | Models: shapes described in text, built as Geometry Script dynamic meshes and attached to a ball or hat slot |
| `src/host/testchannel.*` | Test and measurement commands, from the console and from the tools below |
| `src/proxy/` | The `version.dll` export stubs (generated from the system DLL's export table) |
| `plugins/` | The bundled plugins: Plugin Manager (footer, console, plugin browser) and Hello World (an example) |
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

- Log: `%LOCALAPPDATA%\Ballest\Saved\PluginManager\host.log` (host and plugins; screenshots from the suite land here too)
- `python tools/symbolize.py` names the host's frames in the newest crash report
- `python tools/dev_session.py commands.txt [--map Map_Track13]` runs test-channel commands against a fresh game
- `python tools/memread.py` (as a library) reads a running game's memory, read-only, to measure layouts
- `python tools/measure_follow.py` samples the replay camera in default and follow 3D modes during a real replay

Test channel commands (one line written to `test_command.txt` next to the log) are listed in
`src/host/testchannel.hpp`. The same commands can be typed in game: footer **plugins** > **console**.

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

