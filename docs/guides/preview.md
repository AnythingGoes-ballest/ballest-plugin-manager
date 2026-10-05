# Previewing windows without the game

The preview runs your plugin with the host's real plugin runtime and API, outside the game, and shows its windows
and footer buttons in a browser. Use it to work on a plugin's UI: an edit to the plugin's files reloads it in about
a second, and there is no game to start.

```bash
tools/preview.sh plugins/plugin-manager ../my-plugin
```

Then open <http://localhost:8790>. The footer is along the bottom, as in the game; click your plugin's footer button
to open its windows. Clicks, typing, sliders, dropdowns, check boxes and keys (Escape, the arrows, letters) reach your
plugin the way the game's would. **text** shows the windows as plain text, **log** the host log.

What the preview runs is real: settings are saved, the plugin browser installs and removes plugins (into the
preview's own folder, `build/preview`), and the registry is the live one unless you pass `--registry <file>`.
What it can't do is anything that needs the game: the race, the editor, the HUD, maps, the cursor, and windows docked
into the game's panels. Game calls return nothing there, as they would before the game is ready.

It is close to the game, not identical: the page uses a stand-in for the game's font (put a copy of the font in
`tools/preview/fonts` to use it; it is the game's, so it is never committed), and sizes are drawn the way the game's
widgets are measured, at 1920 x 1080. Check the result in the game before you publish.

## Tests

Steps run the preview without a browser and print the windows as text wherever a step says `dump`:

```
# plugins: plugins/plugin-manager plugins/hello-world
click plugins
dump
type rain
setting hello-world Every 120
press escape
dump
```

Steps: `click <label>` (`label#2` for the second button with that label, `window:<label>` for a window's button
when the footer has one too), `type [@hint|]<text>`, `submit [@hint|]<text>`, `select <first option> <index>`,
`slider <0..1>`, `press <key>`, `setting <plugin> <variable> <value>`, `wait <seconds>` and `dump`.

`tools/preview/test.sh` runs every `tools/preview/tests/*.steps` against `tools/preview/tests/registry.json` and
compares what it prints with the `.txt` beside it; `--update` writes the new text, to read in the diff before
committing.
