#!/usr/bin/env bash
# Writes a moving preview (a GIF) of one Ambient Background pattern, and
# optionally a still of its first frame.
# Usage: tests/make-pattern-preview.sh <out.gif> "<pattern> <frames> <fps> <detail> <size> <colour1> <colour2> <colour3>" [still.png]
# Colours are 0xBBGGRR in hex, 0 for the pattern's default. Needs the test
# build (tests/run-tests.sh) and its test images in build-tests/out or out.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
gif="$1"
spec="$2"
still="${3:-}"
img="$root/out"
[ -f "$img/test-art.png" ] || img="$root/build-tests/out"
frames="$(mktemp -d)"
trap 'rm -rf "$frames"' EXIT
mkdir -p "$frames/G"
STAGEKIT_PREVIEW_PATTERN="$spec" QT_QPA_PLATFORM=offscreen xvfb-run -a -s "-screen 0 1280x720x24" \
  "$root/build-tests/tests/stagekit-ambient-test" "$root/build-tests/stagekit.so" "$root/data" \
  "$img/test-art.png" "$img/test-texture.png" "$img/test-game.png" "$frames/f" \
  "$img/test-title.png" "$frames/G" "$img/test-pillarbox.png" "$img/test-black.png" "$img/test-dark-game.png" |
  grep "ms a frame" || true
python3 - "$frames/f" "$gif" "$spec" "$still" <<'PY'
import glob, struct, sys
from PIL import Image
frames = []
for path in sorted(glob.glob(sys.argv[1] + "/frame-*.rgba")):
    with open(path, "rb") as f:
        w, h = struct.unpack("<II", f.read(8))
        frames.append(Image.frombytes("RGBA", (w, h), f.read(w * h * 4)).convert("RGB").resize((640, 360), Image.LANCZOS))
fps = int(sys.argv[3].split()[2])
if sys.argv[4]:
    frames[0].save(sys.argv[4])
pal = [f.quantize(96, method=Image.MEDIANCUT, dither=Image.NONE) for f in frames]
pal[0].save(sys.argv[2], save_all=True, append_images=pal[1:], duration=1000 // fps, loop=0, optimize=True)
print("wrote", sys.argv[2], len(frames), "frames")
PY
