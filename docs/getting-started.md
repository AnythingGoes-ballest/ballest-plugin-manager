# Your first plugin

In this tutorial you'll write a plugin that shows a small window during races, counting how many seconds you've been
racing, with a button to reset it. It takes about ten minutes.

## 1. Install the plugin manager

Follow the [install steps](https://github.com/AnythingGoes-ballest/ballest-plugin-manager#install), then
start the game once. You should see **plugins** in the footer at the bottom of the main menu.

The game folder is `...\steamapps\common\Ballest of Them All\Ballest\Binaries\Win64`. From here on, **the plugins
folder** means the `plugins` folder inside it. In game, footer **plugins**, then **open plugins folder**
at the top right of the menu, opens it for you.

## 2. Make the folder

Make a folder for your plugin inside the plugins folder. Its name is the plugin's **id**: lowercase, with dashes
between words.

```
plugins\
    race-clock\
        info.toml
        main.as
```

## 3. Describe it: `info.toml`

```toml
[meta]
name = "Race Clock"
version = "0.1.0"
author = "your name"
description = "Seconds spent racing, with a reset button."

[script]
files = ["main.as"]
```

Every key is explained in [info.toml](reference/manifest.md).

## 4. Say hello: `main.as`

Start with the smallest possible plugin:

```cpp
void Main()
{
    Log::Info("race clock loaded");
}
```

`Main()` runs once, when the plugin loads. Start the game (or restart it), then open the log: footer **plugins** >
**console**. You'll find:

```
[info] [race-clock] loaded Race Clock 0.1.0
[info] [race-clock] race clock loaded
```

The console shows the last 400 lines of the log and updates as new lines arrive. If your plugin has a mistake, the
compiler's message appears here too, with the file and line.

!!! tip
    The whole log is also saved to `%LOCALAPPDATA%\Ballest\Saved\PluginManager\host.log`, started fresh each time
    the game starts. It's useful for older lines, or when the game won't start.

## 5. Show a window

Plugins build their user interface once, in `Main()`, and keep the handles (the `@` variables) to change it later.

```cpp
UI::Window@ window;
UI::Text@ clockText;

void Main()
{
    @window = UI::CreateWindow();
    window.SetAnchor(0, 0);        // top left of the screen
    window.SetPivot(0, 0);
    window.SetOffset(40, 200);
    @clockText = window.AddText("0 s", 40);
}
```

Restart the game and the window appears in the top left corner.

## 6. Count while racing

`Update(float dt)` runs every frame. `dt` is how long the last frame took, in seconds. `Race::IsActive()` tells
you whether a race is running.

```cpp
double seconds = 0;

void Update(float dt)
{
    window.visible = Race::OnTrack();      // only on a track, not in the menus
    if (Race::IsActive())
        seconds += dt;
    clockText.text = int(seconds) + " s";
}
```

## 7. Add a reset button

Add the button in `Main()`, under the text:

```cpp
UI::Button@ resetButton;

// in Main(), after AddText:
    window.NewRow();
    @resetButton = window.AddButton("reset");
```

...and react to it in `Update()`:

```cpp
    if (resetButton.Clicked())
        seconds = 0;
```

A button can only be clicked while the mouse cursor is on screen, for example in the pause menu.

## The whole plugin

```cpp
// Race Clock: seconds spent racing, with a reset button.

UI::Window@ window;
UI::Text@ clockText;
UI::Button@ resetButton;
double seconds = 0;

void Main()
{
    @window = UI::CreateWindow();
    window.SetAnchor(0, 0);
    window.SetPivot(0, 0);
    window.SetOffset(40, 200);
    @clockText = window.AddText("0 s", 40);
    window.NewRow();
    @resetButton = window.AddButton("reset");
    Log::Info("race clock loaded");
}

void Update(float dt)
{
    window.visible = Race::OnTrack();
    if (Race::IsActive())
        seconds += dt;
    if (resetButton.Clicked())
        seconds = 0;
    clockText.text = int(seconds) + " s";
}
```

## Where next

- Keep the count across restarts of the game: [Saving data and timing](guides/storage.md).
- Let players change the text size: [Settings](guides/settings.md).
- Let players drag the window: `window.movable = true`. See [Windows and widgets](guides/ui.md).
- Share it: [Publishing a plugin](guides/publishing.md).
