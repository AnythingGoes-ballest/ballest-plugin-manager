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

The tag is the version number with a `v`, and must match `version` in `info.toml`. A tag is all that's needed; you
don't have to make a GitHub release:

```
git tag v0.1.0
git push origin v0.1.0
```

## 3. Ask for it to be added

Two things are never accepted in the registry (checked automatically, see `tools/review_guard.py`):

- **Built binaries**: no `.dll`, `.exe`, archives or other compiled files. Only source and data (pictures, sounds,
  3D models) can be reviewed.
- **`Console::`**: plugins must use the API, not the host's console commands, which reach any game function and get
  around what the API allows. If you need something the API doesn't have, open an issue asking for it.


Open an issue on the [plugin manager repo](https://github.com/AnythingGoes-ballest/ballest-plugin-manager/issues)
with your repo and the tag. The maintainer reviews the code and adds it:

```
python tools/registry.py add ../your-plugin-repo v0.1.0
```

That records the tag's commit and every file's hash in `registry.json`. Once it's pushed, the plugin shows up in
everyone's browser the next time the registry loads.

## Several plugins in one repo

A repo can also hold several plugins, each in its own folder with the same files as above:

```
plugins/
  grind-stats/     info.toml  main.as  icon.png  README.md
  other-plugin/    ...
LICENSE
```

Each plugin is versioned on its own, so its tags start with its id: `grind-stats-v0.1.0`. In the issue, give the
plugin's folder as well as the repo and the tag. It's added with `--path`:

```
python tools/registry.py add ../your-plugins-repo grind-stats-v0.1.0 --repo you/your-plugins-repo --path plugins/grind-stats
```

The plugin's id is its folder's name. Plugins in a shared repo need plugin manager 0.12.0 or later, which the
registry entry's `min_host` asks for by itself.

## Plugins with cooked content

A plugin's repo must not contain built or packed files (see the rules above), and that includes cooked Unreal content.
A plugin that needs its own materials (see [Cooked content](../custom-assets-recipe.md)) keeps its Unreal content
project's *scripts* in its repo. The maintainers build the container from those scripts, attach it to the plugin's
GitHub release, and pin it in the registry by its SHA-256. Ask in your pull request: release files are only accepted
for plugins the maintainers build.

## Updates

An update works like the first version: a new tag, then a request. You don't need to make a GitHub release; the tag is
all the registry uses.

1. Make your changes and raise `version` in `info.toml`.
2. Commit and push, then tag that commit with the new version and push the tag:

    ```
    git tag v0.2.0
    git push origin v0.2.0
    ```

    In a repo with several plugins the tag starts with the plugin's id, as before: `grind-stats-v0.2.0`.

3. Open an issue with the new tag (and the plugin's folder, for a shared repo). The maintainer reviews what changed
   since the last version and adds it the same way:

    ```
    python tools/registry.py add ../your-plugin-repo v0.2.0
    ```

Pushing new commits or a new tag doesn't reach players by itself: the registry keeps pointing at the reviewed commit
until the new version is added.

Once it's added, players who have the plugin see **update** on its card in the browser. The new version is downloaded
and checked in full before it replaces the old one, so a failed download leaves the old version working. Files the
new version no longer lists are removed. What the plugin saved with `Storage` and its settings are kept, since they're
stored outside the plugin's folder.

Never move a tag or reuse a version number you've already published: the registry holds that commit's file hashes,
and installs of that version would fail the check. Fix mistakes with a new version instead.

## Before you publish

- [ ] It loads with no compile errors or warnings in the log.
- [ ] It stays well inside its time budget: no stopped status after an hour of play.
- [ ] It saves with `Storage` every few seconds at most, not every frame.
- [ ] Its windows don't cover the game's own UI by default, and can be hidden.
- [ ] The README says what it does, with a screenshot.
