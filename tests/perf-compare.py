#!/usr/bin/env python3
"""Compare collection-perf JSON, preserving each trial and its sample counts.

Usage: python3 -I tests/perf-compare.py [--strict] [--abba] before=a.json after=b.json [...]

--strict rejects missing/partial measurements and incompatible workloads.
--abba expects four files in baseline, candidate, candidate, baseline order and
reports the mean and range of trial statistics, not a pooled frame percentile.
No timing change is a pass/fail threshold on a shared CI runner.
"""
import argparse
import json
import math
import statistics
import sys


PROFILES = ("obs_graphics_thread", "render_video", "tick_sources", "output_frame")
OPERATIONS = ("fit_own", "fit_obstacle", "ambient_draws", "ambient_sources", "shadow_scale",
              "switch_follow", "link_follow", "item_add", "item_remove", "item_visible",
              "item_transform", "reorder", "refresh")
SHADOW_OPERATIONS = ("shadow_preparations", "shadow_reuses", "shadow_passes")


def load(arg):
    label, _, path = arg.partition("=")
    if not path:
        label, path = arg, arg
    with open(path, encoding="utf-8") as stream:
        return label, json.load(stream)


def change(now, was):
    return f"{(now - was) / was * 100.0:+.1f}%" if was > 0 else "n/a"


def number(value, places=3):
    return f"{value:.{places}f}" if value is not None else "n/a"


def kinds_of(measured):
    took = measured.get("stagekit", {})
    frames = max(1, int(took.get("frames", 0)))
    result = {}
    for kind in took.get("kinds", []):
        calls = int(kind.get("tick_calls", 0))
        renders = int(kind.get("render_calls", 0))
        if not calls and not renders:
            continue
        name = kind.get("id", "")
        name = name[9:] if name.startswith("stagekit_") else name
        tick_us = float(kind.get("tick_us", 0.0))
        result[name] = {
            "inst": calls / frames,
            "per_frame": tick_us / frames,
            "without_long": (tick_us - float(kind.get("tick_long_us", 0.0))) / frames,
            "long": int(kind.get("tick_long_calls", 0)),
            "mean": tick_us / calls if calls else 0.0,
            "p50": kind.get("tick_p50_us"),
            "p95": kind.get("tick_p95_us"),
            "max": kind.get("tick_max_us"),
            "renders": renders,
            "render_p50": kind.get("render_p50_us"),
            "render_p95": kind.get("render_p95_us"),
        }
    return result


def measurement(data, scene):
    return next((row for row in data.get("measurements", []) if row.get("scene") == scene), None)


def finite(value):
    return isinstance(value, (int, float)) and math.isfinite(value) and value >= 0


def validate(runs, expected_scenes):
    reference = None
    for label, data in runs:
        def require(ok, message):
            if not ok:
                raise ValueError(f"{label}: {message}")

        require(data.get("schema_version") == 2, "requires collection-perf schema 2")
        require(data.get("complete") is True and data.get("failures") == 0, "run did not complete")
        require(data.get("stand_ins") == 0, "stand-ins are not allowed in this synthetic comparison")
        require(data.get("loaded_sources", 0) == data.get("expected_sources") and
                data.get("loaded_sources", 0) > 0, "collection did not load completely")
        requested = data.get("requested_frames", 0)
        require(isinstance(requested, int) and requested >= 60, "invalid requested frame count")
        rows = data.get("measurements", [])
        names = [row.get("scene") for row in rows]
        require(names and all(names) and len(set(names)) == len(names), "missing or duplicate scenes")
        if expected_scenes:
            require(names == expected_scenes, f"expected scenes {expected_scenes}, found {names}")
        workload = {key: data.get(key) for key in ("collection", "activity", "canvas_width", "canvas_height",
                                                  "requested_frames", "warmup_seconds", "target_fps",
                                                  "expected_sources", "obs_version")}
        workload["scenes"] = names
        if reference is None:
            reference = workload
        else:
            require(workload == reference, "workload/settings differ from the first run")
        for row in rows:
            scene = row["scene"]
            require(row.get("complete") is True and row.get("requested_frames") == requested,
                    f"{scene}: partial measurement")
            require(row.get("ticked_frames", 0) >= requested, f"{scene}: insufficient completed frames")
            require(row.get("total_frames", 0) - row.get("lagged_frames", 0) == row.get("ticked_frames"),
                    f"{scene}: inconsistent frame counters")
            require(finite(row.get("elapsed_seconds")) and row["elapsed_seconds"] > 0,
                    f"{scene}: invalid duration")
            require(finite(row.get("effective_fps")) and row["effective_fps"] > 0,
                    f"{scene}: invalid throughput")
            for profile in PROFILES:
                values = row.get(profile, {})
                require(values.get("calls", 0) + 2 >= requested, f"{scene}: incomplete {profile} samples")
                require(all(finite(values.get(key)) for key in ("median_ms", "p95_ms", "p99_ms", "mean_ms")),
                        f"{scene}: invalid {profile} timings")
                require(values["median_ms"] <= values["p95_ms"] <= values["p99_ms"],
                        f"{scene}: invalid {profile} percentiles")
            took = row.get("stagekit", {})
            require(took.get("frames", 0) + 2 >= requested and took.get("kinds"),
                    f"{scene}: incomplete StageKit samples")
            require(all(finite(took.get(key)) for key in ("frame_tick_p50_us", "frame_tick_p95_us")),
                    f"{scene}: invalid StageKit percentiles")
            require(took["frame_tick_p50_us"] <= took["frame_tick_p95_us"],
                    f"{scene}: StageKit p95 is below p50")
            require(finite(row.get("stagekit_tick_us_per_frame")), f"{scene}: invalid StageKit tick total")
            require(took.get("operations_available") is True, f"{scene}: missing operation counters")
            counts = took.get("operation_counts", {})
            require(all(finite(counts.get(key)) for key in OPERATIONS), f"{scene}: invalid operation counts")
            if took.get("shadow_operations_available") is True:
                require(all(finite(counts.get(key)) for key in SHADOW_OPERATIONS),
                        f"{scene}: invalid shadow operation counts")


def print_abba(rows):
    print("  ABBA summary: mean [min, max] of the two trial statistics in each group")
    metrics = [(f"{name} {percentile}", "ms", lambda row, name=name, percentile=percentile:
                row.get(name, {}).get(percentile))
               for name in ("obs_graphics_thread", "render_video", "tick_sources")
               for percentile in ("median_ms", "p95_ms")]
    metrics += [
        ("StageKit ticks/frame", "us", lambda row: row.get("stagekit_tick_us_per_frame")),
        ("StageKit frame tick p95", "us", lambda row: row.get("stagekit", {}).get("frame_tick_p95_us")),
    ]
    for title, unit, value in metrics:
        samples = [value(row) for row in rows]
        if any(sample is None for sample in samples):
            continue
        baseline = [samples[0], samples[3]]
        candidate = [samples[1], samples[2]]
        before, after = statistics.mean(baseline), statistics.mean(candidate)
        print(f"    {title:<35} baseline {before:.3f} [{min(baseline):.3f}, {max(baseline):.3f}] {unit}; "
              f"candidate {after:.3f} [{min(candidate):.3f}, {max(candidate):.3f}] {unit}; "
              f"change {change(after, before)}")
    print("    These are repeated trial summaries, not pooled percentiles or a statistical confidence interval.")


def report(runs, abba):
    scenes = list(dict.fromkeys(row.get("scene") for _, data in runs for row in data.get("measurements", [])))
    for scene in scenes:
        print(f"== {scene}")
        present = [(label, measurement(data, scene)) for label, data in runs if measurement(data, scene)]
        for label, row in present:
            took = row.get("stagekit", {})
            print(f"  {label}: profiler frames={row.get('frames', 'n/a')}, "
                  f"ticked={row.get('ticked_frames', 'n/a')}, lagged={row.get('lagged_frames', 'n/a')}, "
                  f"elapsed={number(row.get('elapsed_seconds'))} s, fps={number(row.get('effective_fps'), 2)}")
            for profile in PROFILES:
                values = row.get(profile, {})
                print(f"    {profile:<20} p50 {number(values.get('median_ms')):>9}  "
                      f"p95 {number(values.get('p95_ms')):>9}  p99 {number(values.get('p99_ms')):>9}  "
                      f"mean {number(values.get('mean_ms')):>9} ms  calls={values.get('calls', 'n/a')}")
            kinds = kinds_of(row)
            without_long = sum(kind["without_long"] for kind in kinds.values())
            long_calls = sum(kind["long"] for kind in kinds.values())
            print(f"    StageKit ticks/frame: {number(row.get('stagekit_tick_us_per_frame'))} us, "
                  f"p50 {number(took.get('frame_tick_p50_us'))}, p95 {number(took.get('frame_tick_p95_us'))} us; "
                  f"excluding >=1ms calls {without_long:.3f} us/frame ({long_calls} long calls retained above)")
        if abba:
            print_abba([row for _, row in present])
        all_kinds = list(dict.fromkeys(name for _, row in present
                                      for name, _ in sorted(kinds_of(row).items(), key=lambda item: -item[1]["per_frame"])))
        print("  Per kind: tick p50/p95 and inclusive render p50/p95 are microseconds per callback")
        for name in all_kinds:
            print(f"    {name}")
            for label, row in present:
                kind = kinds_of(row).get(name)
                if not kind:
                    print(f"      {label:<12} no callbacks")
                    continue
                print(f"      {label:<12} ticks/frame {kind['inst']:5.1f}, tick us/frame {kind['per_frame']:9.3f}, "
                      f"tick p50/p95 {number(kind['p50'])}/{number(kind['p95'])}, "
                      f"render calls {kind['renders']}, "
                      f"render p50/p95 {number(kind['render_p50'])}/{number(kind['render_p95'])}")
        print("  Operations: count / per-frame / per-second; Fit counts are logical readback attempts")
        for operation in OPERATIONS + SHADOW_OPERATIONS:
            cells = []
            for label, row in present:
                took = row.get("stagekit", {})
                count = took.get("operation_counts", {}).get(operation)
                per_frame = took.get("operations_per_frame", {}).get(operation)
                per_second = took.get("per_second", {}).get(operation)
                if operation == "ambient_draws" and took.get("ambient_frames_available") is not True:
                    count = per_frame = per_second = None
                if operation in SHADOW_OPERATIONS and took.get("shadow_operations_available") is not True:
                    count = per_frame = per_second = None
                cells.append(f"{label} {number(count, 0)}/{number(per_frame)}/{number(per_second)}")
            print(f"    {operation:<17} " + " | ".join(cells))
        print("    Shadow preparation counters: n/a means that plugin does not expose the optional probe;")
        print("    passes count shadow/halo preparation draws, not final composition or all GPU operations.")
        print("    Operation samples sit just outside timing snapshots; an in-flight frame may cross a boundary.")
        print()
    print("OBS graphics/render/tick metrics cover the full measured scene. Per-kind render callbacks include children")
    print("and overlap, so they are not summed into a global render cost. Tick histograms are StageKit's existing")
    print("approximate histograms. Software-rendered CI results do not establish hardware GPU gains. Shared-runner")
    print("timing variation is reported, not used as a pass/fail threshold.")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--abba", action="store_true")
    parser.add_argument("--expect-scene", action="append", default=[])
    parser.add_argument("runs", nargs="+")
    args = parser.parse_args(argv)
    try:
        runs = [load(arg) for arg in args.runs]
        if args.abba and len(runs) != 4:
            raise ValueError("--abba needs four runs in baseline, candidate, candidate, baseline order")
        if args.strict or args.abba:
            validate(runs, args.expect_scene)
        report(runs, args.abba)
    except (OSError, ValueError, TypeError, KeyError) as error:
        print(f"Invalid performance measurements: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
