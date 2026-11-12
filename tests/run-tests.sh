#!/usr/bin/env bash
# Builds StageKit with tests and runs the headless render test under Xvfb.
# Usage: tests/run-tests.sh [output dir]
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/build-tests/out}"
cmake -S "$root" -B "$root/build-tests" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSTAGEKIT_BUILD_TESTS=ON -DCMAKE_COMPILE_WARNING_AS_ERROR=ON >/dev/null
cmake --build "$root/build-tests"
mkdir -p "$out/game" "$out/title"
python3 "$root/tests/make-test-image.py" "$out"
export QT_QPA_PLATFORM=offscreen
for kind in game title; do
  echo "=== $kind ==="
  xvfb-run -a -s "-screen 0 1280x720x24" \
    "$root/build-tests/tests/stagekit-render-test" "$root/build-tests/stagekit.so" "$root/data" \
    "$out/test-$kind.png" "$out/$kind"
done
python3 "$root/tests/make-contact-sheet.py" "$out/game" "$out/blur-types-game.png" checker
python3 "$root/tests/make-contact-sheet.py" "$out/title" "$out/blur-types-title.png" dark
