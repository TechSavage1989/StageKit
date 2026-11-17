#!/usr/bin/env bash
# Measures what scene collections cost a frame, headless, and compares them.
# Usage: tests/run-perf.sh <build dir> <old.json> <trial.json> [<new.json>]
# Environment: FRAMES (ticked frames per measurement, default 1800), WARMUP
# (seconds, default 15: the first look at every item, the fits and the glances), OUT (folder for the results, default a new one under
# /tmp).
#
# Each collection is measured three times, each in a fresh process under
# Xvfb with stand-ins for the sources this machine cannot load: at rest on
# its full-screen and its component scene, with the captures shown in turn,
# and with the scenes changed in turn with the collection's own transition.
# The first file is the old collection (scenes per game); the others are
# the layout collections (Live, Chat Live, Standby). The owner's collections
# are private: never commit them or the results.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
if [ "$#" -lt 3 ]; then
  echo "usage: $0 <build dir> <old.json> <trial.json> [<new.json>]" >&2
  exit 2
fi
build="$(cd "$1" && pwd)"
shift
frames="${FRAMES:-1800}"
warmup="${WARMUP:-15}"
out="${OUT:-$(mktemp -d /tmp/stagekit-perf.XXXXXX)}"
mkdir -p "$out"
perf="$build/tests/collection-perf"
if [ ! -x "$perf" ]; then
  echo "$perf is not built (configure with -DSTAGEKIT_BUILD_TESTS=ON)" >&2
  exit 1
fi
captures="Game Capture,HD60S,Window Capture,Main Display,Tablet Display Capture"
labels=(old trial new)
results=()
i=0
for file in "$@"; do
  label="${labels[$i]}"
  if [ "$i" -eq 0 ]; then
    rest=(--scene Game-Capture-Scene --scene Game-Component-Capture-Scene)
    first=(--scene Game-Capture-Scene)
    cycle="Game-Capture-Scene,Game-Component-Capture-Scene,Stream Start"
  else
    rest=(--scene Live --scene "Chat Live")
    first=(--scene Live)
    cycle="Live,Chat Live,Standby"
  fi
  for activity in none toggle scenes; do
    case "$activity" in
      none) scenes=("${rest[@]}"); extra=() ;;
      toggle) scenes=("${first[@]}"); extra=(--toggle "$captures") ;;
      scenes) scenes=("${first[@]}"); extra=(--cycle "$cycle") ;;
    esac
    result="$out/$label-$activity.json"
    echo "=== $label: $activity"
    xvfb-run -a -s "-screen 0 1280x720x24" "$perf" "$build/stagekit.so" "$root/data" "$file" \
      --stand-ins --frames "$frames" --warmup "$warmup" --activity "$activity" "${extra[@]}" \
      "${scenes[@]}" --out "$result" --log "$out/$label-$activity.log" 2>/dev/null | grep -v '^libEGL' || true
    [ -f "$result" ] && results+=("$label=$result")
  done
  i=$((i + 1))
done
echo
python3 -I "$root/tests/perf-compare.py" "${results[@]}"
echo "Results: $out"
