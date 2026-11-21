#!/usr/bin/env bash
set -euo pipefail
build=$(realpath "$1")
mkdir -p "$build/filter-results"
export STAGEKIT_COUNTDOWN_CAPTURE="$build/filter-results/countdown-dock.png"
export STAGEKIT_TIMER_EDITOR_CAPTURE="$build/filter-results/countdown-settings.png"
scratch=$(mktemp -d)
export HOME="$scratch/home" XDG_CONFIG_HOME="$scratch/config" STAGEKIT_TEST_REPORT="$scratch/result"
mkdir -p "$HOME" "$XDG_CONFIG_HOME"
cmake --install "$build" --prefix "$scratch/install" > "$scratch/install.log"
binary=$(find "$scratch/install" -name stagekit.so -print -quit)
resources=$(dirname "$(dirname "$(find "$scratch/install" -name stagekit-classic.effect -print -quit)")")
test -f "$resources/components/move/stagekit-port-move.so"
plugins="$XDG_CONFIG_HOME/obs-studio/plugins"
mkdir -p "$plugins/stagekit/bin/64bit" "$plugins/stagekit-replacement-probe/bin/64bit" "$plugins/stagekit-replacement-probe/data"
ln -s "$binary" "$plugins/stagekit/bin/64bit/stagekit.so"
ln -s "$resources" "$plugins/stagekit/data"
ln -s "$build/tests/stagekit-replacement-probe.so" "$plugins/stagekit-replacement-probe/bin/64bit/stagekit-replacement-probe.so"
export QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1
export DEBUGINFOD_URLS=""
obs_args=(--multi --disable-shutdown-check --disable-missing-files-check --disable-updater --verbose)
status=0
timeout 45 xvfb-run -a obs "${obs_args[@]}" > "$scratch/frontend.log" 2>&1 || status=$?
if [[ "$status" == 0 ]] && test -f "$STAGEKIT_TEST_REPORT" && grep -qx PASS "$STAGEKIT_TEST_REPORT"; then
  grep -F -e '[camera transition test]' -e 'StageKit timer at 5x' -e 'StageKit timer dock:' "$scratch/frontend.log"
  echo 'PASS: replacement tools, labeled catalog search and clean OBS shutdown'
  exit 0
fi
echo "OBS frontend exit status: $status"
test ! -f "$STAGEKIT_TEST_REPORT" || cat "$STAGEKIT_TEST_REPORT"
tail -75 "$scratch/frontend.log"
timeout 45 xvfb-run -a gdb -q --batch -ex 'set pagination off' -ex run -ex 'bt 32' --args obs "${obs_args[@]}" > "$scratch/debug.log" 2>&1 || true
grep -Ei 'received signal|#([0-9]+)[ ]|error:' "$scratch/debug.log" | tail -70 || true
exit 1
