#!/usr/bin/env bash
# Builds StageKit with tests and runs the headless render test under Xvfb.
# Usage: tests/run-tests.sh [output dir]
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/build-tests/out}"
cmake -S "$root" -B "$root/build-tests" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSTAGEKIT_BUILD_TESTS=ON -DCMAKE_COMPILE_WARNING_AS_ERROR=ON >/dev/null
# Every key of the locale file once: OBS keeps one text for a key used twice.
dupes="$(grep -E '^[A-Za-z]' "$root/data/locale/en-US.ini" | cut -d= -f1 | sort | uniq -d)"
if [ -n "$dupes" ]; then
  echo "data/locale/en-US.ini has keys more than once:" >&2
  echo "$dupes" >&2
  exit 1
fi
# A failed build must stop here, or the tests would run an older build.
build_exit=0
cmake --build "$root/build-tests" || build_exit=$?
if [ "$build_exit" -ne 0 ]; then
  echo "The build failed (exit code $build_exit); no tests were run." >&2
  exit "$build_exit"
fi
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

echo "=== tooltips ==="
# Its own run: it opens a window of every StageKit kind, which the other
# sections need not share.
mkdir -p "$out/tooltips"
STAGEKIT_TEST_ONLY_TOOLTIPS=1 xvfb-run -a -s "-screen 0 1280x720x24" \
  "$root/build-tests/tests/stagekit-ambient-test" "$root/build-tests/stagekit.so" "$root/data" \
  "$out/test-art.png" "$out/test-texture.png" "$out/test-game.png" "$out/tooltips" \
  "$out/test-title.png" "$out/tooltips" "$out/test-pillarbox.png" "$out/test-black.png" "$out/test-dark-game.png"

# Its own run: an animated standby image plays (it needs out/test-anim.gif).
STAGEKIT_TEST_ONLY_STANDBY_GIF=1 xvfb-run -a -s "-screen 0 1280x720x24" \
  "$root/build-tests/tests/stagekit-ambient-test" "$root/build-tests/stagekit.so" "$root/data" \
  "$out/test-art.png" "$out/test-texture.png" "$out/test-game.png" "$out/tooltips" \
  "$out/test-title.png" "$out/tooltips" "$out/test-pillarbox.png" "$out/test-black.png" "$out/test-dark-game.png"

# Its own run: Ambient Background's patterns, each at a set time.
rm -rf "$out/ambient-patterns"
mkdir -p "$out/ambient-patterns"
STAGEKIT_TEST_ONLY_AMBIENT_PATTERNS=1 xvfb-run -a -s "-screen 0 1280x720x24" \
  "$root/build-tests/tests/stagekit-ambient-test" "$root/build-tests/stagekit.so" "$root/data" \
  "$out/test-art.png" "$out/test-texture.png" "$out/test-game.png" "$out/tooltips" \
  "$out/test-title.png" "$out/tooltips" "$out/test-pillarbox.png" "$out/test-black.png" "$out/test-dark-game.png"
python3 "$root/tests/make-sheet.py" "$out/ambient-patterns" "$out/ambient-patterns.png" 5
# Its own run: the hexagon and dot patterns, as backgrounds and as outline fills.
rm -rf "$out/hex-patterns"
mkdir -p "$out/hex-patterns"
STAGEKIT_TEST_ONLY_HEX_PATTERNS=1 xvfb-run -a -s "-screen 0 1280x720x24" \
  "$root/build-tests/tests/stagekit-ambient-test" "$root/build-tests/stagekit.so" "$root/data" \
  "$out/test-art.png" "$out/test-texture.png" "$out/test-game.png" "$out/tooltips" \
  "$out/test-title.png" "$out/tooltips" "$out/test-pillarbox.png" "$out/test-black.png" "$out/test-dark-game.png"
python3 "$root/tests/make-sheet.py" "$out/hex-patterns" "$out/hex-patterns.png" 4

echo "=== ambient background, shadow & backdrop ==="
rm -rf "$out/ambient" "$out/shadow" "$out/corners" "$out/glow" "$out/morph" "$out/item-morph" "$out/make-room" \
  "$out/darkest"
mkdir -p "$out/ambient" "$out/shadow" "$out/corners" "$out/glow" "$out/morph" "$out/item-morph" "$out/make-room" \
  "$out/darkest"
xvfb-run -a -s "-screen 0 1280x720x24" \
  "$root/build-tests/tests/stagekit-ambient-test" "$root/build-tests/stagekit.so" "$root/data" \
  "$out/test-art.png" "$out/test-texture.png" "$out/test-game.png" "$out/ambient" \
  "$out/test-title.png" "$out/shadow" "$out/test-pillarbox.png" "$out/test-black.png" "$out/test-dark-game.png"
rm -f "$out/ambient/still.rgba" "$out/ambient/moving.rgba" "$out/ambient/start_follow.rgba" "$out/ambient/start_reset.rgba"
python3 "$root/tests/make-sheet.py" "$out/ambient" "$out/ambient-background.png" 4
python3 "$root/tests/make-sheet.py" "$out/shadow" "$out/shadow-backdrop.png" 2 light
python3 "$root/tests/make-sheet.py" "$out/corners" "$out/corners-edges.png" 3 light
python3 "$root/tests/make-sheet.py" "$out/glow" "$out/glow-outline.png" 3 light
python3 "$root/tests/make-sheet.py" "$out/morph" "$out/morph-transition.png" 5
python3 "$root/tests/make-sheet.py" "$out/item-morph" "$out/morph-items.png" 3
python3 "$root/tests/make-sheet.py" "$out/make-room" "$out/make-room.png" 5
python3 "$root/tests/make-sheet.py" "$out/darkest" "$out/ambient-darkest.png" 4
# ==== scene-filters:shadow ====
python3 "$root/tests/make-sheet.py" "$out/scene-shadow" "$out/scene-shadow.png" 2
# Its own run: outline corners, matching widths, outlines through Morph, the glow.
rm -rf "$out/outline-glow"
mkdir -p "$out/outline-glow"
STAGEKIT_TEST_ONLY_OUTLINE_GLOW=1 xvfb-run -a -s "-screen 0 1280x720x24" \
  "$root/build-tests/tests/stagekit-ambient-test" "$root/build-tests/stagekit.so" "$root/data" \
  "$out/test-art.png" "$out/test-texture.png" "$out/test-game.png" "$out/tooltips" \
  "$out/test-title.png" "$out/tooltips" "$out/test-pillarbox.png" "$out/test-black.png" "$out/test-dark-game.png"
python3 "$root/tests/make-sheet.py" "$out/outline-glow" "$out/outline-glow.png" 3 light
# Its own run: the outline's designs, still and moving, from both filters.
rm -rf "$out/outline-designs"
mkdir -p "$out/outline-designs"
STAGEKIT_TEST_ONLY_OUTLINE_DESIGNS=1 xvfb-run -a -s "-screen 0 1280x720x24" \
  "$root/build-tests/tests/stagekit-ambient-test" "$root/build-tests/stagekit.so" "$root/data" \
  "$out/test-art.png" "$out/test-texture.png" "$out/test-game.png" "$out/tooltips" \
  "$out/test-title.png" "$out/tooltips" "$out/test-pillarbox.png" "$out/test-black.png" "$out/test-dark-game.png"
python3 "$root/tests/make-sheet.py" "$out/outline-designs" "$out/outline-designs.png" 4 light
# Its own run: the outline filled with a pattern, from both filters.
rm -rf "$out/outline-patterns" "$out/outline-patterns-same"
mkdir -p "$out/outline-patterns"
STAGEKIT_TEST_ONLY_OUTLINE_PATTERNS=1 xvfb-run -a -s "-screen 0 1280x720x24" \
  "$root/build-tests/tests/stagekit-ambient-test" "$root/build-tests/stagekit.so" "$root/data" \
  "$out/test-art.png" "$out/test-texture.png" "$out/test-game.png" "$out/tooltips" \
  "$out/test-title.png" "$out/tooltips" "$out/test-pillarbox.png" "$out/test-black.png" "$out/test-dark-game.png"
python3 "$root/tests/make-sheet.py" "$out/outline-patterns" "$out/outline-patterns.png" 6 light
# Its own run: the outline filled with an Ambient Background, from both filters.
rm -rf "$out/outline-ambient"
mkdir -p "$out/outline-ambient"
STAGEKIT_TEST_ONLY_OUTLINE_AMBIENT=1 xvfb-run -a -s "-screen 0 1280x720x24" \
  "$root/build-tests/tests/stagekit-ambient-test" "$root/build-tests/stagekit.so" "$root/data" \
  "$out/test-art.png" "$out/test-texture.png" "$out/test-game.png" "$out/tooltips" \
  "$out/test-title.png" "$out/tooltips" "$out/test-pillarbox.png" "$out/test-black.png" "$out/test-dark-game.png"
python3 "$root/tests/make-sheet.py" "$out/outline-ambient" "$out/outline-ambient.png" 4 light
# Its own run: Position X, Position Y and Rotation, in Ambient Background and the outlines.
rm -rf "$out/pattern-transform"
mkdir -p "$out/pattern-transform"
STAGEKIT_TEST_ONLY_PATTERN_TRANSFORM=1 xvfb-run -a -s "-screen 0 1280x720x24" \
  "$root/build-tests/tests/stagekit-ambient-test" "$root/build-tests/stagekit.so" "$root/data" \
  "$out/test-art.png" "$out/test-texture.png" "$out/test-game.png" "$out/tooltips" \
  "$out/test-title.png" "$out/tooltips" "$out/test-pillarbox.png" "$out/test-black.png" "$out/test-dark-game.png"
# ==== end scene-filters:shadow ====
# ==== scene-filters:standby-corners ====
python3 "$root/tests/make-sheet.py" "$out/scene-corners" "$out/scene-corners.png" 2
# ==== end scene-filters:standby-corners ====
# ==== scene-filters:blur ====
python3 "$root/tests/make-sheet.py" "$out/scene-blur" "$out/scene-blur.png" 3
python3 "$root/tests/make-sheet.py" "$out/blur-edges" "$out/blur-edges.png" 3
# ==== end scene-filters:blur ====
# ==== scene-filters:collection ====
# ==== end scene-filters:collection ====
