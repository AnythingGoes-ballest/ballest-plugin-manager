# Publishing a plugin

Players install plugins from the in-game browser, which lists everything in the **registry**:
[`registry.json`](https://github.com/AnythingGoes-ballest/ballest-plugin-manager/blob/main/registry.json) in the
plugin manager's repo.

## How the registry keeps players safe

Each registry entry is pinned to one **commit** of the plugin's GitHub repo, and lists the **SHA-256** of every file.
The game downloads each file from that exact commit and checks it against the hash before writing anything. A
changed file, or a changed repo, can't reach players until a new version is added to the registry, so every version
players get is one that was reviewed.

## 1. Put your plugin in its own GitHub repo

The repo holds what the plugin folder holds, at the top level:

```
info.toml
main.as          (and any other files listed in info.toml)
icon.png         (optional: 256x256; without one the browser shows a default icon)
README.md
LICENSE
```

In `info.toml`, set a `description` (shown in the browser), and `min_host` if you use API added in a later version of
the plugin manager (see [info.toml](../reference/manifest.md)).

## 2. Tag a version

The tag is the version number with a `v`, and must match `version` in `info.toml`:

```
git tag v0.1.0
git push origin v0.1.0
```

## 3. Ask for it to be added

Open an issue on the [plugin manager repo](https://github.com/AnythingGoes-ballest/ballest-plugin-manager/issues)
with your repo and the tag. The maintainer reviews the code and adds it:

```
python tools/registry.py add ../your-plugin-repo v0.1.0
```

That records the tag's commit and every file's hash in `registry.json`. Once it's pushed, the plugin shows up in
everyone's browser the next time the registry loads.

## Updates

Make your changes, raise `version` in `info.toml`, tag the new version, and ask again. Players who have the plugin see
**update** in the browser.

## Before you publish

- [ ] It loads with no compile errors or warnings in the log.
- [ ] It stays well inside its time budget: no stopped status after an hour of play.
- [ ] It saves with `Storage` every few seconds at most, not every frame.
- [ ] Its windows don't cover the game's own UI by default, and can be hidden.
- [ ] The README says what it does, with a screenshot.
