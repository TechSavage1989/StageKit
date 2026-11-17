#!/usr/bin/env bash
# Measures what a scene collection costs a frame, headless, per StageKit kind.
# Usage: tests/run-perf.sh <build dir> [<label>=<collection.json> ...]
#
# With no collection, the synthetic one (tests/make-perf-collection.py: the
# shape of a large StageKit setup, nobody's own sources) is written, on a
# canvas a quarter the size (SCALE, default 0.25: llvmpipe then draws about
# 40 frames a second, so timers come round about as often a frame as in
# OBS), and measured. Each collection is measured in a fresh process under Xvfb, on each
# of SCENES in turn and with "(off)" (an empty scene on the output, so every
# scene is off screen), then the files are compared side by side
# (tests/perf-compare.py), the first as the reference.
#
# Environment: FRAMES (frames per measurement, default 900), WARMUP (seconds
# on a scene first, default 8), SCENES (comma-separated, default
# "Live,Chat Live,Standby,(off)"), OUT (folder for the results, default a new
# one under /tmp), STAND_INS=1 (replace sources of kinds not loaded here, for
# a real collection), PLUGIN (the stagekit.so to measure, default the build
# dir's). The owner's collections are private: never commit them or their
# results.
#
# llvmpipe draws a 2560x1440 frame in a few tenths of a second, and a
# filter that reads a picture back in its tick (Fit to Space's glances) waits
# for that drawing: read the p50 and p95 columns for steady costs, the mean
# and max for those waits.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
if [ "$#" -lt 1 ]; then
  echo "usage: $0 <build dir> [<label>=<collection.json> ...]" >&2
  exit 2
fi
build="$(cd "$1" && pwd)"
shift
frames="${FRAMES:-900}"
warmup="${WARMUP:-8}"
scenes="${SCENES:-Live,Chat Live,Standby,(off)}"
plugin="${PLUGIN:-$build/stagekit.so}"
out="${OUT:-$(mktemp -d /tmp/stagekit-perf.XXXXXX)}"
mkdir -p "$out"
perf="$build/tests/collection-perf"
if [ ! -x "$perf" ]; then
  echo "$perf is not built (configure with -DSTAGEKIT_BUILD_TESTS=ON)" >&2
  exit 1
fi
runs=("$@")
if [ "${#runs[@]}" -eq 0 ]; then
  python3 -I "$root/tests/make-perf-collection.py" "$out/synthetic.json" --scale "${SCALE:-0.25}"
  runs=("synthetic=$out/synthetic.json")
fi
scene_args=()
IFS=',' read -r -a scene_list <<<"$scenes"
for s in "${scene_list[@]}"; do
  scene_args+=(--scene "$s")
done
extra=()
[ "${STAND_INS:-0}" = 1 ] && extra+=(--stand-ins)
results=()
for run in "${runs[@]}"; do
  label="${run%%=*}"
  file="${run#*=}"
  echo "=== $label"
  xvfb-run -a -s "-screen 0 1280x720x24" "$perf" "$plugin" "$root/data" "$file" --frames "$frames" \
    --warmup "$warmup" "${scene_args[@]}" "${extra[@]}" --out "$out/$label.json" --log "$out/$label.log" \
    2>/dev/null | grep -v '^libEGL' || true
  [ -f "$out/$label.json" ] && results+=("$label=$out/$label.json")
done
echo
if [ "${#results[@]}" -gt 0 ]; then
  python3 -I "$root/tests/perf-compare.py" "${results[@]}"
fi
echo "Results: $out"
