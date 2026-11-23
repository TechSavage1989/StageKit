#!/usr/bin/env bash
# Same-runner, sequential baseline/candidate comparison after native/frontend tests.
# Usage: bash tests/check-render-performance.sh build-ports
# Optional STAGEKIT_PERF_OUT, STAGEKIT_PERF_BASELINE, STAGEKIT_PERF_FRAMES (450),
# STAGEKIT_PERF_WARMUP (6), STAGEKIT_PERF_SCALE (auto, 0.25 or 0.125).
# The default baseline is the beta250 main merge, not a moving branch.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
if [[ "$#" != 1 ]]; then
  echo "usage: $0 <candidate build directory>" >&2
  exit 2
fi
build=$(realpath "$1")
out=${STAGEKIT_PERF_OUT:-$build/performance-results}
baseline_ref=${STAGEKIT_PERF_BASELINE:-7a7bdf3ae7d6780b2a0896aa8136a67cdb399ac7}
frames=${STAGEKIT_PERF_FRAMES:-450}
warmup=${STAGEKIT_PERF_WARMUP:-6}
scale=${STAGEKIT_PERF_SCALE:-auto}
perf="$build/tests/collection-perf"
for command in git cmake ninja python3 pkg-config xvfb-run timeout; do
  command -v "$command" >/dev/null || { echo "Missing benchmark dependency: $command" >&2; exit 1; }
done
test -x "$perf"
test -s "$build/stagekit.so"
test -s "$build/CMakeCache.txt"
mkdir -p "$out"
out=$(realpath "$out")
python3 - "$out" "$frames" "$warmup" "$scale" <<'PY'
import math, pathlib, sys
out, frames, warmup, scale = sys.argv[1:]
if any(pathlib.Path(out).iterdir()):
    raise SystemExit("Choose a fresh STAGEKIT_PERF_OUT; previous trials are not overwritten.")
if int(frames) < 60 or not math.isfinite(float(warmup)) or float(warmup) < 0 or scale not in ("auto", "0.25", "0.125"):
    raise SystemExit("Require frames >= 60, warmup >= 0 and scale auto, 0.25 or 0.125.")
PY
candidate_sha=$(git -C "$root" rev-parse HEAD)
if [[ -n "${STAGEKIT_PERF_CANDIDATE_SHA:-}" && "$candidate_sha" != "$STAGEKIT_PERF_CANDIDATE_SHA" ]]; then
  echo "Candidate checkout differs from the requested commit" >&2
  exit 1
fi
export LIBGL_ALWAYS_SOFTWARE=1 QT_QPA_PLATFORM=offscreen
baseline_dir=""
stage=metadata
finish() {
  status=$?
  set +e
  python3 - "$out" "$stage" "$status" <<'PY'
import datetime, json, pathlib, sys
out = pathlib.Path(sys.argv[1])
path = out / "metadata.json"
if path.exists():
    data = json.loads(path.read_text())
    data.update(finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                last_stage=sys.argv[2], exit_code=int(sys.argv[3]), complete=int(sys.argv[3]) == 0)
    data["renderer_log_lines"] = sorted({line for log in out.glob("*.log")
                                         for line in log.read_text(errors="replace").splitlines()
                                         if "llvmpipe" in line.lower() or "OpenGL loaded" in line
                                         or "Loading up OpenGL" in line})
    path.write_text(json.dumps(data, indent=2) + "\n")
PY
  metadata_status=$?
  if [[ "$status" == 0 && "$metadata_status" != 0 ]]; then status=$metadata_status; fi
  if [[ -n "$baseline_dir" && -d "$baseline_dir" ]]; then
    git -C "$root" worktree remove --force "$baseline_dir"
  fi
  exit "$status"
}
trap finish EXIT
python3 - "$root" "$build" "$out" "$baseline_ref" "$candidate_sha" "$frames" "$warmup" "$scale" <<'PY'
import datetime, json, os, pathlib, platform, subprocess, sys
root, build, out, baseline, candidate, frames, warmup, scale = sys.argv[1:]
def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()
cpu = pathlib.Path("/proc/cpuinfo").read_text()
model = next((line.split(":", 1)[1].strip() for line in cpu.splitlines() if line.startswith("model name")), "unknown")
keys = ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_SHA", "STAGEKIT_PERF_CANDIDATE_SHA",
        "RUNNER_OS", "RUNNER_ARCH", "ImageOS", "ImageVersion", "LIBGL_ALWAYS_SOFTWARE",
        "QT_QPA_PLATFORM", "LP_NUM_THREADS", "OMP_NUM_THREADS", "MESA_LOADER_DRIVER_OVERRIDE")
metadata = {"started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "complete": False,
            "baseline_requested": baseline, "candidate_commit": candidate,
            "candidate_tracked_changes": run("git", "-C", root, "status", "--porcelain", "--untracked-files=no"),
            "build_type": "RelWithDebInfo", "candidate_build": build,
            "harness": str(pathlib.Path(build) / "tests/collection-perf"),
            "order": ["baseline", "candidate", "candidate", "baseline"],
            "frames_per_scene": int(frames), "warmup_seconds": float(warmup), "scale_requested": scale,
            "kernel": platform.platform(), "cpu_model": model, "logical_cpus": os.cpu_count(),
            "os_release": pathlib.Path("/etc/os-release").read_text(),
            "cmake": run("cmake", "--version"), "libobs": run("pkg-config", "--modversion", "libobs"),
            "environment": {key: os.environ[key] for key in keys if key in os.environ},
            "limitations": ["Linux llvmpipe software renderer; no hardware GPU performance claim.",
                            "Synthetic collections; no real capture, camera, browser or encoding workload.",
                            "ABBA trial statistics are not pooled frame percentiles or confidence intervals.",
                            "No timing threshold is used on the shared runner.",
                            "Per-kind render callbacks overlap and are never summed as global render cost.",
                            "Shadow preparation counters are unavailable in the frozen baseline."]}
(pathlib.Path(out) / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
PY
mapfile -t configuration < <(python3 - "$build/CMakeCache.txt" <<'PY'
import pathlib, sys
cache = {}
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    if line and not line.startswith(("#", "//")) and "=" in line:
        key, value = line.split("=", 1)
        cache[key.split(":", 1)[0]] = value
for key in ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_BUILD_TYPE", "CMAKE_GENERATOR",
            "CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS", "CMAKE_C_FLAGS_RELWITHDEBINFO", "CMAKE_CXX_FLAGS_RELWITHDEBINFO"):
    print(cache.get(key, ""))
PY
)
if [[ "${#configuration[@]}" != 8 || "${configuration[2]}" != RelWithDebInfo || "${configuration[3]}" != Ninja ]]; then
  echo "Expected the existing native Ninja RelWithDebInfo build" >&2
  exit 1
fi
stage=baseline-checkout
if ! git -C "$root" cat-file -e "$baseline_ref^{commit}" 2>/dev/null; then
  git -C "$root" fetch --no-tags origin "$baseline_ref" > "$out/baseline-fetch.log" 2>&1
fi
baseline_sha=$(git -C "$root" rev-parse "$baseline_ref^{commit}")
baseline_dir=$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/stagekit-perf-baseline.XXXXXX")
git -C "$root" worktree add --detach "$baseline_dir" "$baseline_sha" > "$out/baseline-checkout.log" 2>&1
baseline_build="$baseline_dir/build-perf"
stage=baseline-build
echo "Building frozen baseline $baseline_sha using the candidate compiler and flags"
if ! cmake -S "$baseline_dir" -B "$baseline_build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSTAGEKIT_BUILD_TESTS=ON "-DCMAKE_C_COMPILER=${configuration[0]}" "-DCMAKE_CXX_COMPILER=${configuration[1]}" \
    "-DCMAKE_C_FLAGS=${configuration[4]}" "-DCMAKE_CXX_FLAGS=${configuration[5]}" \
    "-DCMAKE_C_FLAGS_RELWITHDEBINFO=${configuration[6]}" "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=${configuration[7]}" \
    > "$out/baseline-configure.log" 2>&1; then
  tail -100 "$out/baseline-configure.log"
  exit 1
fi
if ! cmake --build "$baseline_build" --target stagekit --parallel 4 > "$out/baseline-build.log" 2>&1; then
  tail -100 "$out/baseline-build.log"
  exit 1
fi
python3 - "$out" "$build" "$baseline_build" "$baseline_sha" <<'PY'
import hashlib, json, pathlib, subprocess, sys
out, build, baseline_build = map(pathlib.Path, sys.argv[1:4])
def cache_at(folder):
    cache = {}
    for line in (folder / "CMakeCache.txt").read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            key = key.split(":", 1)[0]
            if key in ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_BUILD_TYPE", "CMAKE_GENERATOR",
                       "CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS", "CMAKE_C_FLAGS_RELWITHDEBINFO", "CMAKE_CXX_FLAGS_RELWITHDEBINFO"):
                cache[key] = value
    return cache
candidate_cache, baseline_cache = cache_at(build), cache_at(baseline_build)
if candidate_cache != baseline_cache:
    raise SystemExit("Baseline and candidate compiler/build settings differ")
path = out / "metadata.json"
data = json.loads(path.read_text())
data.update(baseline_commit=sys.argv[4], build_configuration=candidate_cache,
            compiler_version=subprocess.check_output([candidate_cache["CMAKE_CXX_COMPILER"], "--version"], text=True).strip(),
            binary_sha256={name: hashlib.sha256(file.read_bytes()).hexdigest() for name, file in
                           (("baseline_plugin", baseline_build / "stagekit.so"), ("candidate_plugin", build / "stagekit.so"),
                            ("shared_harness", build / "tests/collection-perf"))})
path.write_text(json.dumps(data, indent=2) + "\n")
PY

check_idle() {
  python3 - <<'PY'
import pathlib
blocked = []
for proc in pathlib.Path("/proc").iterdir():
    if not proc.name.isdigit():
        continue
    try:
        name = (proc / "comm").read_text().strip()
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        continue
    if name in ("obs", "obs64", "collection-perf", "cc1", "cc1plus", "clang", "clang++", "ninja") or name.startswith("stagekit-"):
        blocked.append(f"{proc.name}:{name}")
if blocked:
    raise SystemExit("Refusing to benchmark while OBS/tests/compiler processes are active: " + ", ".join(blocked))
PY
}
run_measurement() {
  local role=$1 label=$2 collection=$3 run_frames=$4 run_warmup=$5
  shift 5
  local plugin="$build/stagekit.so" data="$root/data" status=0
  if [[ "$role" == baseline ]]; then plugin="$baseline_build/stagekit.so"; data="$baseline_dir/data"; fi
  local scene_args=() expected=()
  for scene in "$@"; do scene_args+=(--scene "$scene"); expected+=(--expect-scene "$scene"); done
  check_idle
  stage=$label
  echo "Measuring $label ($role): $run_frames frames per scene after $run_warmup seconds warmup"
  timeout --signal=TERM --kill-after=15s 600s xvfb-run -a -s "-screen 0 1280x720x24" \
    "$perf" "$plugin" "$data" "$collection" --frames "$run_frames" --warmup "$run_warmup" \
    "${scene_args[@]}" --out "$out/$label.json" --log "$out/$label.log" \
    > "$out/$label-console.log" 2>&1 || status=$?
  if [[ "$status" != 0 ]]; then
    tail -100 "$out/$label-console.log"
    if [[ -f "$out/$label.log" ]]; then tail -100 "$out/$label.log"; fi
    echo "Measurement $label failed with status $status" >&2
    exit "$status"
  fi
  python3 -I "$root/tests/perf-compare.py" --strict "${expected[@]}" "$label=$out/$label.json" \
    > "$out/$label-validation.txt"
}

if [[ "$scale" == auto ]]; then
  stage=calibration
  python3 -I "$root/tests/make-perf-collection.py" "$out/calibration-collection.json" --scale 0.25 --scene-fit
  run_measurement baseline calibration "$out/calibration-collection.json" 60 2 Live
  scale=$(python3 - "$out/calibration.json" <<'PY'
import json, sys
sample = json.load(open(sys.argv[1], encoding="utf-8"))["measurements"][0]
print("0.125" if sample["obs_graphics_thread"]["median_ms"] > 25 or sample["effective_fps"] < 40 else "0.25")
PY
)
fi
echo "Shared measured scale: $scale (selected before all ABBA trials)"
python3 -I "$root/tests/make-perf-collection.py" "$out/scene-fit-collection.json" --scale "$scale" --scene-fit
python3 -I "$root/tests/make-shadow-perf-collection.py" "$out/shadow-blur-collection.json" --scale "$scale"
python3 - "$out" "$scale" <<'PY'
import hashlib, json, pathlib, sys
out = pathlib.Path(sys.argv[1])
collection = json.loads((out / "scene-fit-collection.json").read_text())
if not any(f.get("id") == "stagekit_scene_fit" for source in collection["sources"] for f in source.get("filters", [])):
    raise SystemExit("Scene Fit workload has no Scene Fit filter")
path = out / "metadata.json"
data = json.loads(path.read_text())
data.update(scale=float(sys.argv[2]), calibration_policy="Use 0.125 if baseline quarter-scale graphics p50 >25ms or throughput <40fps.",
            collection_sha256={file.name: hashlib.sha256(file.read_bytes()).hexdigest() for file in out.glob("*-collection.json")})
path.write_text(json.dumps(data, indent=2) + "\n")
PY
for trial in A1 B1 B2 A2; do
  role=baseline
  if [[ "$trial" == B* ]]; then role=candidate; fi
  run_measurement "$role" "scene-fit-$trial" "$out/scene-fit-collection.json" "$frames" "$warmup" Live '(off)'
  run_measurement "$role" "shadow-blur-$trial" "$out/shadow-blur-collection.json" "$frames" "$warmup" 'Shadow Blur'
  python3 - "$out" "$trial" "$role" <<'PY'
import json, pathlib, sys
out, trial, role = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
fit = json.loads((out / f"scene-fit-{trial}.json").read_text())
for measured in fit["measurements"]:
    if not any(kind.get("id") == "stagekit_scene_fit" and kind.get("tick_calls", 0) > 0
               for kind in measured["stagekit"]["kinds"]):
        raise SystemExit(f"{trial}/{measured['scene']}: Scene Fit was not exercised")
shadow = json.loads((out / f"shadow-blur-{trial}.json").read_text())["measurements"][0]["stagekit"]
for required in ("stagekit_scene_shadow", "stagekit_scene_blur"):
    if not any(kind.get("id") == required and kind.get("render_calls", 0) > 0 for kind in shadow["kinds"]):
        raise SystemExit(f"{trial}: {required} did not render")
if role == "candidate":
    counts = shadow["operation_counts"]
    if shadow.get("shadow_operations_available") is not True or any(
            counts.get(key, 0) <= 0 for key in ("shadow_preparations", "shadow_reuses", "shadow_passes")):
        raise SystemExit(f"{trial}: focused workload did not exercise prepared shadow reuse")
PY
done
stage=report
for workload in scene-fit shadow-blur; do
  python3 -I "$root/tests/perf-compare.py" --strict --abba \
    "A1=$out/$workload-A1.json" "B1=$out/$workload-B1.json" "B2=$out/$workload-B2.json" "A2=$out/$workload-A2.json" \
    > "$out/$workload-summary.txt"
  cat "$out/$workload-summary.txt"
done
stage=complete
echo "Complete measurements and metadata: $out"
