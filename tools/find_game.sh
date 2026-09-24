# Finding Ballest of Them All, shared by install.sh and uninstall.sh (sourced, not run).
# Steam's install folder (from the registry, then the usual places), every Steam library listed in
# steamapps/libraryfolders.vdf, and the one holding Ballest's app manifest (app 3339810).

APP_ID=3339810
GAME_EXE="Ballest-Win64-Shipping.exe"

to_unix() { cygpath -u "$1" 2>/dev/null || printf '%s' "$1"; }

steam_roots() {
  local key value
  for key in 'HKCU\Software\Valve\Steam' 'HKLM\SOFTWARE\WOW6432Node\Valve\Steam' 'HKLM\SOFTWARE\Valve\Steam'; do
    for value in SteamPath InstallPath; do
      reg query "$key" //v "$value" 2>/dev/null | sed -n "s/.*$value *REG_SZ *//p" | tr -d '\r'
    done
  done
  printf '%s\n' "C:/Program Files (x86)/Steam" "C:/Program Files/Steam"
}

libraries() {
  local root vdf
  while IFS= read -r root; do
    [ -n "$root" ] || continue
    root=$(to_unix "$root")
    [ -d "$root/steamapps" ] || continue
    printf '%s\n' "$root"
    vdf="$root/steamapps/libraryfolders.vdf"
    # lines like:  "path"   "D:\\SteamLibrary"
    [ -f "$vdf" ] && sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf" | sed 's/\\\\/\\/g' | tr -d '\r' |
      while IFS= read -r lib; do to_unix "$lib"; printf '\n'; done
  done < <(steam_roots)
}

find_game() {
  local lib dir
  while IFS= read -r lib; do
    [ -n "$lib" ] || continue
    if [ -f "$lib/steamapps/appmanifest_$APP_ID.acf" ]; then
      dir=$(sed -n 's/^[[:space:]]*"installdir"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$lib/steamapps/appmanifest_$APP_ID.acf" | tr -d '\r')
      dir="$lib/steamapps/common/${dir:-Ballest of Them All}/Ballest/Binaries/Win64"
      [ -f "$dir/$GAME_EXE" ] && { printf '%s\n' "$dir"; return 0; }
    fi
  done < <(libraries | awk '!seen[$0]++')
  return 1
}

# True while the game is running (it keeps version.dll open).
game_running() {
  tasklist //FI "IMAGENAME eq $GAME_EXE" //FO CSV //NH 2>/dev/null | grep -qi "$GAME_EXE"
}

# Sets GAME to the game's Binaries\Win64 folder: the one given, or the one found. Exits if there is none.
locate_game() {
  if [ -z "$GAME" ]; then
    GAME=$(find_game) || fail "could not find Ballest of Them All in any Steam library; pass --game \"<...\Ballest\Binaries\Win64>\""
  fi
  GAME=$(to_unix "$GAME")
  [ -f "$GAME/$GAME_EXE" ] || fail "$GAME does not contain $GAME_EXE"
  say "game folder: $GAME"
  if game_running; then
    [ "${DRY_RUN:-0}" = 1 ] && say "note: Ballest is running; close it before doing this for real" ||
      fail "Ballest is running; close it first (the running game keeps version.dll open)"
  fi
}
