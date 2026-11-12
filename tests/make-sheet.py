"""Lays out every .rgba frame in a folder as one labelled PNG (sorted by name).

Usage: make-sheet.py <frames dir> <out.png> [columns] [light]
"light" shows transparent areas over a pale blue-grey so shadows are visible.
"""
import glob, os, struct, sys
from PIL import Image, ImageDraw, ImageFont

frames_dir, out_png = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 4
light = len(sys.argv) > 4 and sys.argv[4] == "light"
FONT = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 15)
frames = []
for path in sorted(glob.glob(os.path.join(frames_dir, "*.rgba"))):
    with open(path, "rb") as f:
        w, h = struct.unpack("<II", f.read(8))
        img = Image.frombytes("RGBA", (w, h), f.read(w * h * 4))
    base = Image.new("RGBA", (w, h), (120, 150, 190, 255) if light else (0, 0, 0, 255))
    frames.append((os.path.basename(path)[:-5].replace("_", " "), Image.alpha_composite(base, img).convert("RGB")))
tw, th, pad, label_h = (480, 270, 8, 22) if light else (320, 180, 8, 22)
rows = (len(frames) + cols - 1) // cols
sheet = Image.new("RGB", (pad + cols * (tw + pad), pad + rows * (th + label_h + pad)), (16, 16, 20))
d = ImageDraw.Draw(sheet)
for i, (name, img) in enumerate(frames):
    x, y = pad + (i % cols) * (tw + pad), pad + (i // cols) * (th + label_h + pad)
    d.text((x, y + 2), name, font=FONT, fill=(235, 235, 240))
    img.thumbnail((tw, th), Image.LANCZOS)
    sheet.paste(img, (x, y + label_h))
sheet.save(out_png)
print("wrote", out_png)
