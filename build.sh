#!/usr/bin/env bash
# Builds build/version.dll (the host) with the bundled llvm-mingw.
#   ./build.sh            build
#   ./build.sh install    build, then copy version.dll and plugins/ next to the game exe
# Every build also writes build/version.sym.dll: the same code unstripped, for tools/symbolize.py.
set -euo pipefail
cd "$(dirname "$0")"
CXX=tools/llvm-mingw/bin/x86_64-w64-mingw32-clang++
AS=third_party/angelscript
# Strict warnings for our code; AngelScript is a system include so its own warnings stay out of the way.
FLAGS="-std=c++20 -O2 -g1 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -isystem $AS/include -isystem $AS/add_on"
mkdir -p build/as build/host
# AngelScript is compiled once; delete build/as to rebuild it.
if [ ! -f build/as/.done ]; then
  for f in $AS/source/*.cpp $AS/add_on/scriptstdstring/scriptstdstring.cpp $AS/add_on/scriptstdstring/scriptstdstring_utils.cpp $AS/add_on/scriptarray/scriptarray.cpp $AS/add_on/scriptbuilder/scriptbuilder.cpp; do
    $CXX -std=c++17 -O2 -w -I$AS/include -c "$f" -o build/as/$(basename "$f" .cpp).o
  done
  touch build/as/.done
fi
rm -f build/host/*.o                  # no stale objects from renamed or removed sources
for f in src/host/*.cpp src/proxy/exports.cpp; do
  $CXX $FLAGS -c "$f" -o build/host/$(basename "$f" .cpp).o
done
$CXX -c src/proxy/exports.S -o build/host/exports_stubs.o
$CXX -shared -static -o build/version.sym.dll build/host/*.o build/as/*.o src/proxy/version.def -lkernel32 -luser32
tools/llvm-mingw/bin/llvm-strip -o build/version.dll build/version.sym.dll
echo "built build/version.dll ($(stat -c %s build/version.dll) bytes)"
if [ "${1:-}" = "install" ]; then
  # The game's Binaries\Win64 folder; set BALLEST_GAME_DIR if Steam keeps it somewhere else.
  GAME="${BALLEST_GAME_DIR:-/c/Program Files (x86)/Steam/steamapps/common/Ballest of Them All/Ballest/Binaries/Win64}"
  cp build/version.dll "$GAME/version.dll"
  # Only the plugins that ship with the host are replaced: ones installed from the registry, and the kill switch
  # (plugins/DISABLED), stay as they are.
  mkdir -p "$GAME/plugins"
  for dir in plugins/*/; do
    name=$(basename "$dir")
    rm -rf "$GAME/plugins/$name" && cp -r "$dir" "$GAME/plugins/$name"
  done
  echo "installed version.dll and the bundled plugins to $GAME"
fi
