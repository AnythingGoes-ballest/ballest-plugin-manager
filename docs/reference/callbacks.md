# Callbacks

Functions the host calls in your script. Define the ones you need, with exactly these declarations. See
[How plugins run](../concepts.md) for the time budget they run under.

### Main

```cpp
void Main()
```

Runs once, after the plugin has been compiled and its saved [settings](../guides/settings.md) have been put into
their variables. Build the UI and load saved data here.

??? example "Example"
    ```cpp
    UI::Window@ window;
    UI::Text@ timeText;

    void Main()
    {
        // Saved data first, then the UI that shows it
        seconds = parseFloat(Storage::Get("seconds", "0"));
        @window = UI::CreateWindow();
        @timeText = window.AddText(FormatTime(seconds), TimeSize);
        OnSettingsChanged();        // apply the saved settings to the new widgets
    }
    ```

### Update

```cpp
void Update(float dt)
```

Runs every frame. `dt` is the length of the last frame in seconds. Read what the player did, update what's shown,
count time.

??? example "Example"
    ```cpp
    void Update(float dt)
    {
        // Count racing time, and show it only on a track
        window.visible = Race::OnTrack();
        if (Race::IsActive())
            seconds += dt;
        timeText.text = FormatTime(seconds);
    }
    ```

### OnSettingsChanged

```cpp
void OnSettingsChanged()
```

Runs after the player changes one or more of your settings, before your next `Update`. The new values are already
in the setting variables.

??? example "Example"
    ```cpp
    [Setting name="Time size" min=16 max=160]
    float TimeSize = 56;

    void OnSettingsChanged()
    {
        // Text sizes are only read when set, so apply the new one
        timeText.size = TimeSize;
    }
    ```

### OnDisabled

```cpp
void OnDisabled()
```

Runs once, just before the plugin's script is freed: when the player turns it off in the plugin manager, when it's
removed, or when a plugin it depends on is turned off or removed. Requires host 0.12.0.

The host already takes away your windows, panels, footer buttons, cursor request, leaderboard notes and editor
additions. Use `OnDisabled` for what it can't know about: changes you made to the game itself, such as HUD layouts,
a cosmetic you equipped, or objects you changed through the console. This is your script's last call, so nothing you
leave for a later `Update` will happen. `Console::Run` commands still run, because the host runs them itself.

It isn't called when the plugin is stopped by an error or by going over its time budget, or when the game closes.

??? example "Example"
    ```cpp
    void OnDisabled()
    {
        // Give the game its own timer back
        Hud::ClearLayout("PlayerUI/TimeGroup");
    }
    ```

## Script language basics

Plugins are [AngelScript](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_script.html). Besides the
plugin API, scripts have:

- `string`, with `+` joining text and numbers (`"restarts " + count`), `split` and `join`
- `array<T>`, for example `array<int> ids = {1, 2, 3};` with `length()`, `insertLast()` and `removeAt()`
- `parseInt`, `parseFloat`, `formatInt` and `formatFloat` for converting between numbers and text
