"""Lays out every .rgba frame in a folder as one labelled PNG (sorted by name)."""
import glob, os, struct, sys
from PIL import Image, ImageDraw, ImageFont

frames_dir, out_png = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 4
FONT = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 15)
frames = []
for path in sorted(glob.glob(os.path.join(frames_dir, "*.rgba"))):
    with open(path, "rb") as f:
        w, h = struct.unpack("<II", f.read(8))
        img = Image.frombytes("RGBA", (w, h), f.read(w * h * 4)).convert("RGB")
    frames.append((os.path.basename(path)[:-5].replace("_", " "), img))
tw, th, pad, label_h = 320, 180, 8, 22
rows = (len(frames) + cols - 1) // cols
sheet = Image.new("RGB", (pad + cols * (tw + pad), pad + rows * (th + label_h + pad)), (16, 16, 20))
d = ImageDraw.Draw(sheet)
for i, (name, img) in enumerate(frames):
    x, y = pad + (i % cols) * (tw + pad), pad + (i // cols) * (th + label_h + pad)
    d.text((x, y + 2), name, font=FONT, fill=(235, 235, 240))
    sheet.paste(img.resize((tw, th), Image.LANCZOS), (x, y + label_h))
sheet.save(out_png)
print("wrote", out_png)
