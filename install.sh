#!/usr/bin/env bash
# Installs the Ballest plugin manager (Git Bash on Windows).
#
#   ./install.sh                 finds the game, downloads the latest release, checks it, installs it
#   ./install.sh --from-build    installs this checkout's build/version.dll and plugins/ instead (developers)
#   ./install.sh --game "<dir>"  the game's Ballest\Binaries\Win64 folder, if it cannot be found
#   ./install.sh --dry-run       shows what it would do, changes nothing
#
# The game is found through Steam (tools/find_game.sh). The release's version.dll is checked against the SHA-256 in
# this repo's registry.json before anything is copied. Plugins installed from the in-game browser, and
# plugins\DISABLED, are left alone.
set -euo pipefail

REPO="AnythingGoes-ballest/ballest-plugin-manager"
FROM_BUILD=0 DRY_RUN=0 GAME=""

while [ $# -gt 0 ]; do
  case "$1" in
    --from-build) FROM_BUILD=1 ;;
    --dry-run) DRY_RUN=1 ;;
    --game) GAME="$2"; shift ;;
    -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

say() { printf '%s\n' "$*"; }
fail() { printf 'install: %s\n' "$*" >&2; exit 1; }
run() { if [ $DRY_RUN = 1 ]; then say "  (dry run) $*"; else "$@"; fi; }

. "$(dirname "$0")/tools/find_game.sh"
locate_game

# --- what to install --------------------------------------------------------------------------------------------
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [ $FROM_BUILD = 1 ]; then
  HERE=$(cd "$(dirname "$0")" && pwd)
  [ -f "$HERE/build/version.dll" ] || fail "no build/version.dll here; run ./build.sh first"
  mkdir -p "$WORK/release"
  cp "$HERE/build/version.dll" "$WORK/release/"
  cp -r "$HERE/plugins" "$WORK/release/plugins"
  say "installing this checkout's build"
else
  say "looking up the latest release of $REPO"
  api=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest") || fail "could not reach GitHub"
  tag=$(printf '%s' "$api" | sed -n 's/^[[:space:]]*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)
  zip_url=$(printf '%s' "$api" | sed -n 's/^[[:space:]]*"browser_download_url":[[:space:]]*"\([^"]*\.zip\)".*/\1/p' | head -1)
  [ -n "$tag" ] && [ -n "$zip_url" ] || fail "the latest release has no zip"
  say "latest release: $tag"
  curl -fsSL -o "$WORK/release.zip" "$zip_url" || fail "download failed: $zip_url"
  mkdir -p "$WORK/release"
  powershell -NoProfile -Command "Expand-Archive -LiteralPath '$(cygpath -w "$WORK/release.zip")' -DestinationPath '$(cygpath -w "$WORK/release")' -Force" \
    || fail "could not unpack the release"
  [ -f "$WORK/release/version.dll" ] || fail "the release zip has no version.dll"

  # The DLL must be the one the registry names for this release.
  registry=$(curl -fsSL "https://raw.githubusercontent.com/$REPO/main/registry.json") || fail "could not read registry.json"
  expected=$(printf '%s' "$registry" | tr -d '\n' | sed -n 's/.*"host"[^}]*"dll_sha256":[[:space:]]*"\([0-9a-f]*\)".*/\1/p')
  expected_version=$(printf '%s' "$registry" | tr -d '\n' | sed -n 's/.*"host":[[:space:]]*{[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*/\1/p')
  actual=$(sha256sum "$WORK/release/version.dll" | cut -d' ' -f1)
  if [ "v$expected_version" = "$tag" ]; then
    [ "$actual" = "$expected" ] || fail "version.dll does not match the SHA-256 in registry.json; not installing"
    say "version.dll matches the registry's SHA-256"
  else
    say "note: the registry lists host $expected_version, not $tag, so the DLL is not checked against it"
  fi
fi

# --- install ----------------------------------------------------------------------------------------------------
say "installing into $GAME"
run cp "$WORK/release/version.dll" "$GAME/version.dll"
run mkdir -p "$GAME/plugins"
for dir in "$WORK/release/plugins"/*/; do
  name=$(basename "$dir")
  run rm -rf "$GAME/plugins/$name"
  run cp -r "$dir" "$GAME/plugins/$name"
  say "  plugins/$name"
done
[ -f "$GAME/dwmapi.dll" ] && say "note: dwmapi.dll (UE4SS) is also installed; the plugin manager does not need it"
say "done: start Ballest and look for 'plugins' in the footer"
