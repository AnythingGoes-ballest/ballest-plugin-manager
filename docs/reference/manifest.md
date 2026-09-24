# info.toml

Every plugin folder has an `info.toml` describing the plugin. Only a small part of TOML is used: `[section]`
headings, and `key = "text"`, `key = number` or `key = ["a", "b"]` lines.

```toml
[meta]
name = "Grind Timer"
version = "0.2.1"
author = "AnythingGoes"
description = "A green Trackmania-style timer of total time racing."
min_host = "0.4.0"
icon = "icon.png"

[script]
files = ["main.as"]
timeout = 20
```

## `[meta]`

| Key | Default | Meaning |
|---|---|---|
| `name` | the folder name | The name shown to players. |
| `version` | `"0.0.0"` | The plugin's version. When published, the git tag must be this with a `v` in front. |
| `author` | `""` | Shown in the plugin browser. |
| `description` | `""` | One line, shown in the plugin browser. |
| `min_host` | none | The oldest plugin manager (host) version with the API the plugin uses. An older host won't install it ("error: needs host 0.4.0") or run it (status "needs host 0.4.0"). |
| `icon` | `"icon.png"` | An image file in the plugin folder, 256x256. Without one, the default icon is used. |
| `essential` | `false` | Reserved for the plugin manager itself. |

## `[script]`

| Key | Default | Meaning |
|---|---|---|
| `files` | `["main.as"]` | The script files, compiled together. `#include` isn't supported, so list every file here. |
| `timeout` | `50` | The [time budget](../concepts.md#the-time-budget) for each callback, in milliseconds. |

## The folder name is the id

The plugin's folder name is its **id**: how the registry, `Storage` and the log refer to it. Use lowercase with
dashes (`grind-timer`), and never change it once published: a new id is a different plugin with no saved data.
