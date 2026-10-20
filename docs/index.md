# Ballest plugin docs

The Ballest plugin manager lets anyone add features to **Ballest of Them All** with small scripts called plugins. A
timer, replay controls, extra track editor tools: each is a plugin. Players install them from inside the game
(footer **plugins** > **browse**, with a search that filters as you type), and they start straight away, with no
restart.

Plugins are written in [AngelScript](https://www.angelcode.com/angelscript/), a small scripting language that looks
like C++ or C#. You don't need a compiler, Visual Studio or any engine tools. A text editor is enough.

<div class="grid cards" markdown>

- **[Your first plugin](getting-started.md)**: a plugin that shows a window, in about ten minutes.
- **[How plugins run](concepts.md)**: callbacks, the time budget, and what happens when something goes wrong.
- **[Guides](guides/ui.md)**: windows and widgets, settings, saving data, debugging, publishing, drawing in the
  world.
- **[API reference](reference/api/ui.md)**: every function, each with an example.

</div>

## What a plugin looks like

A folder in the game's `plugins` folder with two files:

=== "info.toml"

    ```toml
    [meta]
    name = "Hello World"
    version = "0.1.0"
    author = "you"
    description = "Says hello."

    [script]
    files = ["main.as"]
    ```

=== "main.as"

    ```cpp
    void Main()
    {
        Log::Info("hello from a plugin");
    }
    ```

## Examples to read

- [Hello World](https://github.com/AnythingGoes-ballest/ballest-plugin-manager/tree/main/plugins/hello-world): the
  smallest plugin.
- [Grind Timer](https://github.com/AnythingGoes-ballest/ballest-grind-timer-plugin): a movable window, settings,
  saved totals.
- [Replay Manager](https://github.com/AnythingGoes-ballest/ballest-replay-manager): replay controls with a slider,
  dropdowns and keyboard input.
- [Create Extensions](https://github.com/AnythingGoes-ballest/ballest-create-extensions): track editor tools (drag to
  select, groups, rotate modes, Shift/Ctrl+click to deselect, rotated copies), a toolbar dropdown and key list rows in the editor's own
  style, and a section docked in the editor's details panel.
- [Cosmetic Kit](https://github.com/AnythingGoes-ballest/ballest-cosmetic-kit) and
  [Example Cosmetics](https://github.com/AnythingGoes-ballest/ballest-example-cosmetics): one plugin offering functions
  to others, and one depending on it to add custom balls, hats and a goal explosion, with models: shapes described in
  text, and 3D model files from Blender (an animated T-rex and skeleton running inside clear balls, a baseball cap).
- [Monkey Balls](https://github.com/AnythingGoes-ballest/ballest-monkey-balls): characters that run inside clear
  balls, with limbs that swing from their joints and keep time with the ball.
- [Fit Window](https://github.com/AnythingGoes-ballest/ballest-fit-window): a few lines that maximize the game's
  window at startup when it doesn't fit the screen.
- [Plugin Manager](https://github.com/AnythingGoes-ballest/ballest-plugin-manager/tree/main/plugins/plugin-manager):
  its Classic theme's plugin browser, settings pages and console, built with cards and a tab header (the other themes
  are drawn by the host). This is the largest example.
