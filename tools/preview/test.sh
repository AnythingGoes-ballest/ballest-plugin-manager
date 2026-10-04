#!/usr/bin/env bash
# Offline UI tests, no game: runs each tools/preview/tests/*.steps through the preview headless and compares what the
# windows show with the .txt beside it. A steps file's first line names the plugin folders it runs
# ("# plugins: plugins/plugin-manager plugins/hello-world"); the registry is tests/registry.json.
#   tools/preview/test.sh            check every test
#   tools/preview/test.sh --update   write what they show now as the expected text (read the diff before committing)
set -euo pipefail
cd "$(dirname "$0")/../.."
tools/preview.sh --build-only
failed=0
for steps in tools/preview/tests/*.steps; do
  expected="${steps%.steps}.txt"
  plugins=$(head -1 "$steps" | sed -n 's/^# plugins: //p')
  actual=$(build/preview.exe $plugins --registry tools/preview/tests/registry.json --data build/preview-test --steps "$steps")
  if [ "${1:-}" = "--update" ]; then
    printf '%s\n' "$actual" > "$expected"
    echo "wrote $expected"
  elif diff -u "$expected" <(printf '%s\n' "$actual"); then
    echo "ok   $steps"
  else
    echo "FAIL $steps"
    failed=1
  fi
done
exit $failed
