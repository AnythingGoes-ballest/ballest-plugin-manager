#!/usr/bin/env bash
# Offline tests (no game): builds the host first, then
#   ascheck        compiles every plugin folder given (default: the bundled ones) against the host's real API
#   ghostdata_test parses the ghost replays the game cached and checks their routes against a reference
#                  (needs the game's GhostSwarmCache and a ghosts.js from Will's viewer: skipped when missing)
#   tools/tests/run.sh [plugin folder]...
set -euo pipefail
cd "$(dirname "$0")/../.."
./build.sh > /dev/null
CXX=tools/llvm-mingw/bin/x86_64-w64-mingw32-clang++
AS=third_party/angelscript
OBJS=$(ls build/host/*.o | grep -v "/main.o\|/exports.o\|/exports_stubs.o")
$CXX -std=c++20 -O1 -isystem $AS/include -isystem $AS/add_on -c tools/tests/ascheck.cpp -o build/ascheck.o
$CXX -static -o build/ascheck.exe build/ascheck.o $OBJS build/as/*.o -lkernel32 -luser32 -lws2_32 -lshell32 -lole32 -luuid
$CXX -std=c++17 -O2 -static -o build/ghostdata_test.exe tools/tests/ghostdata_test.cpp src/host/ghostdata.cpp src/host/json.cpp
if [ $# -eq 0 ]; then set -- plugins/*/; fi
build/ascheck.exe "$@" | grep -v "info: Compiling"
CACHE="$LOCALAPPDATA/Ballest/Saved/GhostSwarmCache/v1"
REF="${GHOSTS_JS:-}"
if [ -d "$CACHE" ] && [ -n "$REF" ]; then
  for dir in "$CACHE"/*/; do build/ghostdata_test.exe "$(cygpath -w "$dir")" "$REF" | tail -1; done
else
  echo "ghostdata_test skipped (set GHOSTS_JS to Will's ghosts.js, with the game's ghost cache present)"
fi
