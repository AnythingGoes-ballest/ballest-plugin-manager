#!/usr/bin/env bash
# Uninstalls the Ballest plugin manager (Git Bash on Windows).
#
#   ./uninstall.sh                 finds the game, removes version.dll and the plugins folder
#   ./uninstall.sh --purge         also deletes saved plugin data (settings, storage, the log)
#   ./uninstall.sh --game "<dir>"  the game's Ballest\Binaries\Win64 folder, if it cannot be found
#   ./uninstall.sh --dry-run       shows what it would do, changes nothing
#
# version.dll is only removed if it is the plugin manager's (it contains "Ballest plugin host"). The game's own files,
# saves and replays are never touched.
set -euo pipefail

PURGE=0 DRY_RUN=0 GAME=""

while [ $# -gt 0 ]; do
  case "$1" in
    --purge) PURGE=1 ;;
    --dry-run) DRY_RUN=1 ;;
    --game) GAME="$2"; shift ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

say() { printf '%s\n' "$*"; }
fail() { printf 'uninstall: %s\n' "$*" >&2; exit 1; }
run() { if [ $DRY_RUN = 1 ]; then say "  (dry run) $*"; else "$@"; fi; }
done_() { [ $DRY_RUN = 1 ] || say "$*"; }     # reports what was really done

. "$(dirname "$0")/tools/find_game.sh"
locate_game

if [ -f "$GAME/version.dll" ]; then
  if grep -aq "Ballest plugin host" "$GAME/version.dll"; then
    run rm -f "$GAME/version.dll"
    done_ "removed version.dll"
  else
    say "version.dll is not the plugin manager's; left alone"
  fi
fi
for old in "$GAME"/version.dll.old-*; do
  [ -e "$old" ] && run rm -f "$old" && done_ "removed $(basename "$old") (left by an update)"
done
if [ -d "$GAME/plugins" ]; then
  run rm -rf "$GAME/plugins"
  done_ "removed the plugins folder"
fi

DATA="$(to_unix "${LOCALAPPDATA:-}")/Ballest/Saved/PluginManager"
if [ -d "$DATA" ]; then
  if [ $PURGE = 1 ]; then
    run rm -rf "$DATA"
    done_ "removed saved plugin data ($DATA)"
  else
    say "kept saved plugin data in $DATA (./uninstall.sh --purge removes it)"
  fi
fi
say "done: the game runs without plugins"
