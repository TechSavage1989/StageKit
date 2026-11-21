"""Create a reproducible RGBA test pattern and labeled contact sheet from real OBS frames."""
from pathlib import Path
import struct
import sys
from PIL import Image, ImageDraw

out = Path(sys.argv[1])
out.mkdir(exist_ok=True, parents=True)
if len(sys.argv) == 2:
    w, h = 320, 180
    art = Image.new("RGBA", (w, h))
    for y in range(26, h - 24):
        for x in range(35, w - 35):
            art.putpixel((x, y), ((x * 3) % 256, (x + y) % 256, (y * 2) % 256, 255))
    draw = ImageDraw.Draw(art)
    for x in range(48, 180, 7):
        draw.rectangle((x, 45, x + 2, 125), fill=(250, 230, 40, 255))
    draw.ellipse((165, 45, 285, 140), fill=(150, 45, 110, 160))
    art.save(out / "art.png")
    mask = Image.new("RGBA", (w, h))
    ImageDraw.Draw(mask).rectangle((0, 0, w // 2, h), fill=(255, 255, 255, 255))
    mask.save(out / "mask.png")
else:
    items = [("original","Original"), ("custom","Custom Effect File"), ("shadow","Dropshadow"),
             ("edges","Edge Detection"), ("gaussian","Gaussian Blur"), ("toon","Toon"),
             ("composite","Composite Blur"), ("saved-shader","Saved drop-shadow shader")]
    sw, sh = 322, 208
    page = Image.new("RGB", (2*sw, 4*sh), "#25272b")
    pen = ImageDraw.Draw(page)
    for i, (file, label) in enumerate(items):
        data = (out / (file+".rgba")).read_bytes()
        w, h = struct.unpack_from("<II", data)
        art = Image.frombytes("RGBA", (w, h), data[8:])
        art.thumbnail((314, 176))
        x, y = (i%2)*sw + 4, (i//2)*sh + 4
        page.paste(art, (x,y+22), art)
        pen.text((x+4,y+3), label, fill="white")
    page.save(out / "filter-replacements.png", optimize=True)
