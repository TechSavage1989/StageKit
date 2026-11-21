#!/usr/bin/env bash
set -euo pipefail
build=$(realpath "$1")
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
timeout 100 xvfb-run -a obs --multi --disable-shutdown-check --disable-missing-files-check --disable-updater --verbose > "$scratch/frontend.log" 2>&1 &
pid=$!
for i in $(seq 1 85); do
  if test -f "$STAGEKIT_TEST_REPORT" || ! kill -0 "$pid" 2>/dev/null; then break; fi
  sleep 1
done
if test -f "$STAGEKIT_TEST_REPORT" && grep -qx PASS "$STAGEKIT_TEST_REPORT"; then
  echo 'PASS: replacement tools, labeled filters and the searchable catalog in the OBS frontend'
  wait "$pid" || true
else
  test ! -f "$STAGEKIT_TEST_REPORT" || cat "$STAGEKIT_TEST_REPORT"
  tail -140 "$scratch/frontend.log"
  exit 1
fi
