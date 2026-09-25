# How plugins run

## Loading

When the game starts, the host looks at every folder in the plugins folder that has an `info.toml`. It compiles each
plugin's scripts into a module of their own, so plugins can't see each other's variables. Then it calls each
plugin's `Main()`.

Plugins that [depend on other plugins](#plugins-that-use-other-plugins) load after them.

A plugin installed from the in-game browser loads straight away, without a restart. A plugin removed there is
stopped and its windows disappear. A folder you create or edit by hand is picked up the next time the game starts.

## Callbacks

The host calls these functions in your script if they exist. All of them are optional.

| Callback | When |
|---|---|
| `void Main()` | Once, after the plugin is loaded. Build your UI and load saved data here. |
| `void Update(float dt)` | Every frame. `dt` is the length of the last frame in seconds. |
| `void OnSettingsChanged()` | After the player changes one of your [settings](guides/settings.md), before your next `Update`. |

Everything happens on the game's own thread, between frames. While your callback runs, the game waits for it. That's
why the time budget below exists.

## The time budget

Each callback has a time budget, 50 milliseconds unless `timeout` in `info.toml` says otherwise. If a callback is
still running when it runs out, the host **stops the plugin** for the rest of the session:

- its status becomes `stopped: exceeded its 50 ms budget` (shown on its card in the plugin manager's **installed** tab, and in
  the log)
- its windows, panels and footer buttons are hidden, so nothing dead is left on screen
- the game and the other plugins carry on

A script error such as using a null handle stops the plugin the same way, with the error and line number as its
status: `exception: Null pointer access in void Update(float) line 10`.

A normal `Update` takes well under a millisecond. To stay inside the budget:

- Do heavy work once, in `Main()`, or spread it over several frames.
- Save with [Storage](reference/api/storage.md) every few seconds, not every frame.
- Time the game spends on work you asked for doesn't count. Pasting and selecting track pieces (`Editor::Select`,
  `Editor::DuplicateSelection`, `Editor::RotatePieces`) is excluded, up to 5 seconds a frame.

## What a plugin can do

A plugin can only do what the API offers. It can't read or write files, open network connections, or call into the
game directly. The API covers:

- **[UI](reference/api/ui.md)**: footer buttons, panels, and windows built from the game's own widgets
- **[Input](reference/api/input.md)**: keys and mouse buttons
- **[Race](reference/api/race.md)**, **[Replay](reference/api/replay.md)** and **[Editor](reference/api/editor.md)**:
  what's happening in the game, and some control over it
- **[Storage](reference/api/storage.md)**: saved text values, private to your plugin
- **[Log](reference/api/log.md)** and **[Host](reference/api/host.md)**: the log, a clock, opening GitHub pages

Installing and removing plugins, updating the host and changing other plugins' settings are for the plugin manager
only.

## Plugins that use other plugins

A plugin can use functions another plugin offers. List that plugin's id under `dependencies` in
[info.toml](reference/manifest.md), and import its functions by the plugin's id:

```angelscript
// info.toml: dependencies = ["cosmetic-kit"]
import bool AddBall(const string &in, const string &in, const string &in, const string &in, const string &in) from "cosmetic-kit";
```

- Plugins load after the plugins they depend on. A plugin whose dependency isn't installed or has stopped doesn't
  start; its status says `needs cosmetic-kit`.
- Installing a plugin from the browser installs its dependencies first.
- Removing or stopping a plugin also stops the plugins that depend on it.
- An imported function runs as the plugin that offers it: its log lines, `Storage` and time budget are that plugin's.
- Any global function can be imported; the offering plugin needs nothing special. Document the ones you mean others
  to use.

## UI handles live as long as the plugin

`UI::CreateWindow()`, `AddText()` and the rest return handles you keep in global variables. They stay valid for as
long as the plugin runs. The game destroys and rebuilds its widgets when it changes menus or loads a map. The host
notices and rebuilds your widgets behind the same handles, so a plugin never has to rebuild its UI after a map
change.

## Turning plugins off

- One plugin, for now: **turn off** on its card in the **installed** tab. It stays installed, with its settings and
  saved data, and stays off after a restart until you **turn on** again. Plugins that depend on it wait for it.
- One plugin, for good: **remove** on its card, or delete its folder.
- All plugins: create an empty file named `DISABLED` in the plugins folder.
- The whole plugin manager: delete `version.dll` from the game folder.

## The game build

The host is measured on one game build. On a newer build it still tries to run: it looks up the few engine tables it
needs in the new exe (logged as "this game build was not measured ... finding the engine's tables again"), and everything else is
found by name as usual. Parts of the game a plugin relies on may have changed, so after a game update some things can
stop working until a plugin manager made for the new build is released.

If something goes wrong in the host's own code:

- while a plugin was running, that plugin is stopped (status `stopped: crashed in host code (...)`) and the others
  carry on;
- otherwise the host turns itself off for the session ("the host crashed ...; plugins are off until the game
  restarts") and the game carries on without plugins.

It tries again on the next launch.
