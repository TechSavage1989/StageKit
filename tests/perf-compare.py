#!/usr/bin/env python3
"""Side by side: what collections cost a frame, from collection-perf's --out files.

Usage: python3 -I tests/perf-compare.py old=old-none.json trial=trial-none.json [new=...] ...

Files with the same activity are compared measurement by measurement, in the order each measured its
scenes (the old collection's full-screen scene beside the trial's Live, and so on). Each line: OBS's
tick_sources and graphics thread (median and 99th percentile, ms), the share of frames over 1 ms, and
StageKit's own ticks in microseconds a frame; then each figure against the first file's.
"""
import json
import sys


def load(arg):
    label, _, path = arg.partition("=")
    if not path:
        label, path = path or arg, arg
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    return label, data


def fig(m, key, what):
    return float(m.get(key, {}).get(what, 0.0))


def change(now, was):
    if was <= 0:
        return "     "
    return f"{(now - was) / was * 100.0:+4.0f}%"


def main(argv):
    if not argv:
        print(__doc__.strip())
        return 2
    runs = [load(a) for a in argv]
    by_activity = {}
    for label, data in runs:
        by_activity.setdefault(data.get("activity", "none"), []).append((label, data))
    names = {"none": "at rest", "toggle": "captures shown in turn", "scenes": "scenes changed in turn"}
    for activity in ("none", "toggle", "scenes"):
        group = by_activity.get(activity)
        if not group:
            continue
        print(f"== {names[activity]}")
        count = max(len(d.get("measurements", [])) for _, d in group)
        for i in range(count):
            first = None
            for label, data in group:
                ms = data.get("measurements", [])
                if i >= len(ms):
                    continue
                m = ms[i]
                row = {
                    "tick": fig(m, "tick_sources", "median_ms"),
                    "tick99": fig(m, "tick_sources", "p99_ms"),
                    "gfx": fig(m, "obs_graphics_thread", "median_ms"),
                    "gfx99": fig(m, "obs_graphics_thread", "p99_ms"),
                    "over": fig(m, "obs_graphics_thread", "over_1ms_pct"),
                    "sk": float(m.get("stagekit_tick_us_per_frame", 0.0)),
                }
                line = (f"  {label:<6} {m.get('scene', '?'):<30} {m.get('frames', 0):>5} fr  "
                        f"tick_sources {row['tick']:7.3f} / {row['tick99']:7.3f}  "
                        f"graphics {row['gfx']:8.3f} / {row['gfx99']:8.3f}  "
                        f"StageKit {row['sk']:9.1f} us")
                if first is None:
                    first = row
                else:
                    line += (f"   vs {group[0][0]}: tick median {change(row['tick'], first['tick'])}, "
                             f"p99 {change(row['tick99'], first['tick99'])}, "
                             f"StageKit {change(row['sk'], first['sk'])}")
                if activity == "scenes" and m.get("crossfades"):
                    line += f"   ({m['crossfades']} Morph cross-fades)"
                print(line)
        print()
    print("(ms, median / 99th percentile, from OBS's own profiler; StageKit: its ticks a frame, from")
    print(" stagekit_timing_take. Stand-ins and llvmpipe: compare the columns, not the absolute times.)")
    # The dearest StageKit kinds, per file, at rest.
    print()
    for label, data in by_activity.get("none", []):
        for m in data.get("measurements", []):
            took = m.get("stagekit", {})
            frames = max(1, int(took.get("frames", 0)))
            kinds = sorted(took.get("kinds", []), key=lambda k: -float(k.get("tick_us", 0.0)))
            parts = []
            for k in kinds[:6]:
                calls = int(k.get("tick_calls", 0))
                if not calls:
                    continue
                kid = k.get("id", "")
                kid = kid[9:] if kid.startswith("stagekit_") else kid
                parts.append(f"{kid} {calls / frames:.0f}x {float(k['tick_us']) / calls:.1f} us")
            print(f"  {label:<6} {m.get('scene', '?'):<30} " + ", ".join(parts))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
