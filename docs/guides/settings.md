# Settings

Settings are global variables with a `[Setting]` tag, the same way Openplanet does them. Each plugin with settings
gets its own settings page in the plugin manager: footer **plugins** > **installed**, then **settings** on
the plugin's card. The page saves the player's changes and tells your plugin about them.

```cpp
[Setting name="Time size" min=16 max=160 description="Height of the time, in pixels"]
float TimeSize = 56;

[Setting name="Show restarts"]
bool ShowRestarts = true;

[Setting name="Label"]
string Label = "grind";
```

The value you give the variable is the **default**. By the time `Main()` runs, the player's saved value (if any) is
already in the variable, so just use it.

## How each type is shown

| Type | Shown as |
|---|---|
| `bool` | an on/off button |
| a number with `min` and `max` | a slider, plus a text box for an exact value |
| any other number, or `string` | a text box (type, then Enter) |

Numbers are kept between `min` and `max`. Allowed types: `bool`, `int`, `uint`, `float`, `double`, `string`.

## Tag attributes

| Attribute | Meaning |
|---|---|
| `name="..."` | What the settings page calls it. Without it, the variable's name is used. |
| `description="..."` | A line of explanation under the name. |
| `min=` and `max=` | The allowed range. With both, the setting gets a slider. |
| `hidden` | Saved, but not shown on the settings page. Useful for values your plugin sets itself. |

## Reacting to changes

When the player changes a setting, the host writes the new value into your variable and calls
`OnSettingsChanged()` before your next `Update()`. Apply anything that doesn't pick the value up by itself:

```cpp
void OnSettingsChanged()
{
    timeText.size = TimeSize;
    restartText.visible = ShowRestarts;
}
```

Call it yourself at the end of `Main()` too, so the first frame uses the saved values.

## Saving

A setting at its default isn't saved. If you change a default in a later version, everyone who never touched that
setting gets the new one. Settings are stored with your plugin's [Storage](../reference/api/storage.md) under
`setting.<variable name>`, so don't use keys starting with `setting.` for anything else.

A plugin can also assign its own setting variables. The change is used straight away but isn't saved: settings are
saved when the player changes them.
