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

echo "=== ambient background, shadow & backdrop ==="
rm -rf "$out/ambient" "$out/shadow"
mkdir -p "$out/ambient" "$out/shadow"
xvfb-run -a -s "-screen 0 1280x720x24" \
  "$root/build-tests/tests/stagekit-ambient-test" "$root/build-tests/stagekit.so" "$root/data" \
  "$out/test-art.png" "$out/test-texture.png" "$out/test-game.png" "$out/ambient" \
  "$out/test-title.png" "$out/shadow" "$out/test-pillarbox.png" "$out/test-black.png" "$out/test-dark-game.png"
rm -f "$out/ambient/still.rgba" "$out/ambient/moving.rgba" "$out/ambient/start_follow.rgba" "$out/ambient/start_reset.rgba"
python3 "$root/tests/make-sheet.py" "$out/ambient" "$out/ambient-background.png" 4
python3 "$root/tests/make-sheet.py" "$out/shadow" "$out/shadow-backdrop.png" 2 light
