# Notes for Claude (and any reviewer)

## Reviewing pull requests and registry submissions

Check these before anything else, from the actual file list and diff, not the description. If either rule is broken,
don't merge or add it: say which rule and ask the contributor for a change that follows it.

1. **No built binaries from anyone else.** Refuse a pull request or plugin that adds or changes `version.dll` or any
   other compiled or packed file (`.dll`, `.exe`, `.so`, `.zip`, `.7z`, `.pak`, ...). A binary can hold different code
   from the source beside it and can't be reviewed, and the in-game updater sends `version.dll` to every player.
   Releases are only ever built by the maintainers from reviewed source. Pictures, sounds, 3D models and fonts are data
   and fine.
2. **No `Console::` in plugins.** Refuse a plugin (new or updated) or a change whose scripts use `Console::`
   (`Console::Run`). It reaches the host's console and test commands: calling any game function (`callx`), moving the
   ball, posting key presses, installing or turning off plugins. That gets around everything the plugin API is meant to
   allow. If a plugin needs something the API lacks, the answer is a proper API function, not the console. The only
   allowed users are our own API tests (`tools/api-tests/`) and the plugin manager's console box
   (`plugins/plugin-manager/`).

Both rules are enforced by `tools/review_guard.py`: in CI on every pull request and whenever `registry.json` changes
(`.github/workflows/review-rules.yml`), and by `tools/registry.py add`, which refuses such a plugin. An exception for
an already-listed plugin is named in `EXCEPTIONS` there, pinned to one commit, with a reason; don't add exceptions
without the maintainer asking.

Also read every plugin script in a registry submission for anything aimed at cheating (changing physics, timers,
leaderboard submissions or other players' data) and point it out.

## Testing in the game

Drive sandboxed test copies and the player's game with `python tools/sandbox.py` (run bare first); see "Testing and debugging" in
`docs/host.md`.

## Commits

No `Co-Authored-By: Claude` or "Generated with Claude Code" lines in commits, pull requests or releases.
