#!/usr/bin/env python3
"""Write a stationary Scene Shadow followed by per-item Scene Blur workload.

Usage: python3 -I tests/make-shadow-perf-collection.py out.json --scale 0.25
The background and two colored panels are ordinary OBS color sources. The
filter settings match the existing Scene Shadow/Scene Blur feature, with no
animation or show/hide transitions. No personal collection is used.
"""
import argparse
import json


def collection(scale):
    width, height = round(2560 * scale), round(1440 * scale)
    sources = []
    items = []
    for index, (name, x, y, w, h, color) in enumerate((
            ("Background", 0, 0, 2560, 1440, 0xFF302820),
            ("Panel Blue", 260, 230, 900, 760, 0xFFE08030),
            ("Panel Orange", 1440, 430, 700, 650, 0xFF3060E0)), start=1):
        sources.append({"id": "color_source", "versioned_id": "color_source_v3", "name": name,
                        "uuid": f"00000000-0000-4000-8001-{index:012d}",
                        "settings": {"width": round(w * scale), "height": round(h * scale), "color": color},
                        "filters": []})
        items.append({"name": name, "id": index, "visible": True, "locked": False,
                      "pos": {"x": x * scale, "y": y * scale}, "scale": {"x": 1.0, "y": 1.0},
                      "rot": 0.0, "align": 5, "bounds_type": 0, "bounds_align": 0,
                      "bounds": {"x": 0.0, "y": 0.0}, "crop_left": 0, "crop_top": 0,
                      "crop_right": 0, "crop_bottom": 0})
    filters = [
        {"id": "stagekit_scene_shadow", "versioned_id": "stagekit_scene_shadow",
         "name": "Scene Shadow (StageKit)", "enabled": True,
         "settings": {"shadow_from": 0, "softness": 20, "shadow_opacity": 60,
                      "ramp_ms": 0, "ramp_out_ms": 0, "outline": True,
                      "outline_width": 6, "outline_design": 5}},
        {"id": "stagekit_scene_blur", "versioned_id": "stagekit_scene_blur",
         "name": "Scene Blur (StageKit)", "enabled": True,
         "settings": {"blur_area": 1, "cover": 0, "blur_type": 0, "strength": 12,
                      "ramp_in_ms": 0, "ramp_out_ms": 0}},
    ]
    sources.append({"id": "scene", "versioned_id": "scene", "name": "Shadow Blur",
                    "uuid": "00000000-0000-4000-8001-000000000004",
                    "settings": {"items": items, "id_counter": len(items), "custom_size": False},
                    "filters": filters})
    return {"name": "StageKit shadow blur performance (synthetic)",
            "current_scene": "Shadow Blur", "current_program_scene": "Shadow Blur",
            "scene_order": [{"name": "Shadow Blur"}], "sources": sources, "groups": [],
            "current_transition": "Cut", "transition_duration": 300,
            "transitions": [{"id": "cut_transition", "name": "Cut", "settings": {}}],
            "resolution": {"x": width, "y": height}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output")
    parser.add_argument("--scale", type=float, default=0.25)
    args = parser.parse_args()
    if args.scale not in (0.25, 0.125):
        parser.error("--scale must be 0.25 or 0.125 for this CI comparison")
    with open(args.output, "w", encoding="utf-8") as stream:
        json.dump(collection(args.scale), stream, indent=2)
        stream.write("\n")


if __name__ == "__main__":
    main()
