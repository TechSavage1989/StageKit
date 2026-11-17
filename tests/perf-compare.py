#!/usr/bin/env python3
"""Side by side: what StageKit's ticks cost a frame, from collection-perf's --out files.

Usage: python3 -I tests/perf-compare.py before=a.json after=b.json [...]

For each scene measured (by name; "(off)" is every scene off screen): OBS's tick_sources median, StageKit's
ticks in microseconds a frame (all kinds, and the per-frame p50 and p95), then each kind's instances, µs a frame
and its per-call mean, p50, p95 and max. Every file after the first is also given as a change against the first.
"""
import json
import sys


def load(arg):
    label, _, path = arg.partition("=")
    if not path:
        label, path = arg, arg
    with open(path, encoding="utf-8") as f:
        return label, json.load(f)


def change(now, was):
    if was <= 0:
        return "    "
    return f"{(now - was) / was * 100.0:+.0f}%"


def kinds_of(m):
    took = m.get("stagekit", {})
    frames = max(1, int(took.get("frames", 0)))
    out = {}
    for k in took.get("kinds", []):
        calls = int(k.get("tick_calls", 0))
        if not calls:
            continue
        kid = k.get("id", "")
        kid = kid[9:] if kid.startswith("stagekit_") else kid
        out[kid] = {
            "inst": calls / frames,
            "per_frame": float(k.get("tick_us", 0.0)) / frames,
            "mean": float(k.get("tick_us", 0.0)) / calls,
            "p50": float(k.get("tick_p50_us", 0.0)),
            "p95": float(k.get("tick_p95_us", 0.0)),
            "max": float(k.get("tick_max_us", 0.0)),
        }
    return out


def measurement(data, scene):
    return next((x for x in data.get("measurements", []) if x.get("scene") == scene), None)


def main(argv):
    if not argv:
        print(__doc__.strip())
        return 2
    runs = [load(a) for a in argv]
    scenes = []
    for _, data in runs:
        for m in data.get("measurements", []):
            if m.get("scene") not in scenes:
                scenes.append(m.get("scene"))
    for scene in scenes:
        print(f"== {scene}")
        first = None
        for label, data in runs:
            m = measurement(data, scene)
            if not m:
                continue
            took = m.get("stagekit", {})
            row = {"tick": float(m.get("tick_sources", {}).get("median_ms", 0.0)),
                   "sk": float(m.get("stagekit_tick_us_per_frame", 0.0)),
                   "p50": float(took.get("frame_tick_p50_us", 0.0)),
                   "p95": float(took.get("frame_tick_p95_us", 0.0))}
            line = (f"  {label:<8} tick_sources median {row['tick']:8.3f} ms   StageKit ticks {row['sk']:9.1f} us a "
                    f"frame (p50 {row['p50']:.1f}, p95 {row['p95']:.1f})")
            if first is None:
                first = row
            else:
                line += (f"   mean {change(row['sk'], first['sk'])}, p50 {change(row['p50'], first['p50'])}, "
                         f"p95 {change(row['p95'], first['p95'])}")
            print(line)
        print(f"  {'kind':<20}" + "".join(f" | {label:^44}" for label, _ in runs))
        print(f"  {'':<20}" + "".join(f" | {'inst':>4} {'us/fr':>7} {'mean':>7} {'p50':>6} {'p95':>6} {'max':>7}"
                                      for _ in runs))
        all_kinds = []
        for _, data in runs:
            m = measurement(data, scene)
            if m:
                for kid, _k in sorted(kinds_of(m).items(), key=lambda kv: -kv[1]["per_frame"]):
                    if kid not in all_kinds:
                        all_kinds.append(kid)
        for kid in all_kinds:
            cells = []
            for _, data in runs:
                m = measurement(data, scene)
                k = kinds_of(m).get(kid) if m else None
                if not k:
                    cells.append(f" | {'':44}")
                    continue
                cells.append(f" | {k['inst']:4.0f} {k['per_frame']:7.2f} {k['mean']:7.2f} {k['p50']:6.2f} "
                             f"{k['p95']:6.2f} {k['max']:7.1f}")
            print(f"  {kid:<20}" + "".join(cells))
        print()
    print("(us: StageKit's own timing, stagekit_timing_take; per kind: instances, us a frame, and per call the mean,")
    print(" p50, p95 and longest. tick_sources: OBS's profiler, which counts every source, not only StageKit's.)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
