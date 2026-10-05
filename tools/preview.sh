#!/usr/bin/env bash
# Builds the host and the preview (tools/preview), then runs the preview: plugins' windows in a browser, no game.
#   tools/preview.sh <plugin folder>... [--port 8790] [--registry live|<registry.json>] [--data <dir>] [--steps <file>]
#   tools/preview.sh plugins/plugin-manager ../ballest-grind-stats      then open http://localhost:8790
#   tools/preview.sh --build-only
set -euo pipefail
cd "$(dirname "$0")/.."
./build.sh > /dev/null
CXX=tools/llvm-mingw/bin/x86_64-w64-mingw32-clang++
AS=third_party/angelscript
OBJS=$(ls build/host/*.o | grep -v "/main.o\|/exports.o\|/exports_stubs.o")
$CXX -std=c++20 -O1 -Wall -Wextra -isystem $AS/include -isystem $AS/add_on -c tools/preview/preview.cpp -o build/preview.o
$CXX -static -o build/preview.exe build/preview.o $OBJS build/as/*.o -lkernel32 -luser32 -lws2_32 -lshell32 -lole32 -luuid
[ "${1:-}" = "--build-only" ] && exit 0
exec build/preview.exe "$@"
