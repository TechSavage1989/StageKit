"""Lays the raw frames written by stagekit-render-test out as one labelled PNG."""
import glob, os, struct, sys
from PIL import Image, ImageDraw, ImageFont

frames_dir, out_png = sys.argv[1], sys.argv[2]
bg = sys.argv[3] if len(sys.argv) > 3 else "checker"
FONT = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 15)

def load(path):
    with open(path, "rb") as f:
        w, h = struct.unpack("<II", f.read(8))
        img = Image.frombytes("RGBA", (w, h), f.read(w * h * 4))
    if bg == "dark":
        base = Image.new("RGBA", (w, h), (24, 26, 34, 255))
    else:
        base = Image.new("RGBA", (w, h), (200, 200, 200, 255))
        d = ImageDraw.Draw(base)
        for y in range(0, h, 16):
            for x in range(0, w, 16):
                if (x // 16 + y // 16) % 2:
                    d.rectangle([x, y, x + 15, y + 15], fill=(150, 150, 150, 255))
    return Image.alpha_composite(base, img).convert("RGB")

rows = [["original"], ["gaussian"], ["kawase"], ["box"], ["pixelate"], ["hexagon"], ["zoom"], ["motion"]]
grid = []
for (prefix,) in rows:
    files = sorted(glob.glob(os.path.join(frames_dir, f"{prefix}*.rgba")))
    if prefix == "original":
        files += [os.path.join(frames_dir, "ramp_mid.rgba")]
    grid.append([(os.path.basename(p)[:-5], load(p)) for p in files if os.path.exists(p)])

tw, th = 320, 180
pad, label_h = 8, 22
cols = max(len(r) for r in grid)
sheet = Image.new("RGB", (pad + cols * (tw + pad), pad + len(grid) * (th + label_h + pad)), (16, 16, 20))
d = ImageDraw.Draw(sheet)
for r, row in enumerate(grid):
    for c, (name, img) in enumerate(row):
        x, y = pad + c * (tw + pad), pad + r * (th + label_h + pad)
        label = name.replace("_", "  strength ").replace("strength 0", "strength ").replace("ramp  strength mid", "mid-ramp (50%)")
        d.text((x, y + 2), label, font=FONT, fill=(235, 235, 240))
        sheet.paste(img.resize((tw, th), Image.LANCZOS), (x, y + label_h))
sheet.save(out_png)
print("wrote", out_png)
