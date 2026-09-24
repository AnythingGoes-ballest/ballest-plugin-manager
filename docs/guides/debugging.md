# Debugging

## The log

Everything goes to one log, which you read in the game: footer **plugins** > **console**. It shows the
last 400 lines and updates live. The whole log is also saved to
`%LOCALAPPDATA%\Ballest\Saved\PluginManager\host.log`, started fresh each time the game starts: use it for older
lines, or when the game won't start.

```
[21:25:25.230] [info] [replay-manager] loaded Replay Manager 0.1.0
[21:26:20.142] [info] [host] race restarted from the beginning (1 this session)
[21:28:03.931] [error] [my-plugin] stopped: exceeded its 50 ms budget
```

Write your own lines with `Log::Info`, `Log::Warn` and `Log::Error`. They're tagged with your plugin's id.

## Compile errors

If a script doesn't compile, the plugin doesn't load, and the compiler's messages are in the log with the file, line
and column:

```
[error] [compiler] my-plugin/main.as (14, 5): <the compiler's message>
```

Fix the file and restart the game.

## A plugin that stopped

Each plugin's card in the plugin manager's **installed** tab shows its status. `running` is fine. Anything else says
why it stopped:

| Status | Cause |
|---|---|
| `stopped: exceeded its N ms budget` | A callback took longer than its [time budget](../concepts.md#the-time-budget): usually a loop that does too much, or never ends. |
| `exception: Null pointer access in ... line N` | A handle that was never set (a missing `@window = ...`), or one used before `Main()` set it. |
| `exception: Index out of bounds ...` | An array read past its end. |
| `off` | Turned off on its card in the plugin manager (**turn on** starts it again). |
| `needs cosmetic-kit` | A plugin in its `dependencies` isn't installed, is turned off, or has stopped. |
| `error: an imported function was not found in its dependencies` | An `import ... from "id"` names a function that plugin doesn't have (the log lists each import). |
| `stopped: crashed in host code (...)` | Something failed inside the host while this plugin was running, usually after a game update. Worth reporting with host.log. |

A stopped plugin stays stopped until the game restarts.

## Common mistakes

**Forgetting `@` when keeping a handle.** `window = UI::CreateWindow();` tries to copy the window. Keep the handle
itself:

```cpp
UI::Window@ window;
void Main() { @window = UI::CreateWindow(); }
```

**Checking a click in two places.** `Clicked()`, `Changed()` and `Submitted()` return `true` once, then clear. If
two parts of your code read the same button, only the first sees the click. Read it once and keep the result:

```cpp
bool reset = resetButton.Clicked();
```

**Saving every frame.** `Storage::Set` writes to disk. See [Saving data and timing](storage.md).

**A hotkey that fires while typing.** It won't: while a text box has focus, `Input` reports no keys.

## The console

The console's text box runs the host's own commands, the same list as `Console::Run`. The useful ones:

| Command | Shows |
|---|---|
| `state` | Your windows' state: which are shown, and their texts. |
| `viewtarget` | The camera the game is looking through. |
| `editor` | The track editor's selected pieces, with positions and rotations. |
| `find <text>` | Game objects and functions whose names contain the text. |

The full list is in [`src/host/testchannel.hpp`](https://github.com/AnythingGoes-ballest/ballest-plugin-manager/blob/main/src/host/testchannel.hpp).
