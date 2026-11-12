"""Generates the test images used by the headless render test."""
import sys
from PIL import Image, ImageDraw, ImageFont

out = sys.argv[1]
W, H = 640, 360
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

# 1. An opaque "game capture" style frame: gradient sky, shapes, HUD text.
img = Image.new("RGBA", (W, H))
d = ImageDraw.Draw(img)
for y in range(H):
    t = y / H
    d.line([(0, y), (W, y)], fill=(int(30 + 60 * t), int(40 + 90 * t), int(120 + 100 * (1 - t)), 255))
d.rectangle([0, 270, W, H], fill=(40, 120, 60, 255))
d.ellipse([470, 40, 560, 130], fill=(255, 220, 90, 255))
for i, c in enumerate([(230, 60, 60), (60, 200, 230), (250, 250, 250), (140, 80, 220)]):
    d.rectangle([60 + i * 90, 170 + (i % 2) * 30, 120 + i * 90, 270], fill=c + (255,))
for x in range(0, 120, 6):  # fine stripes stress aliasing
    d.line([(500 + x // 2, 200), (500 + x // 2, 260)], fill=(255, 255, 255, 255) if x % 12 == 0 else (0, 0, 0, 255))
f = ImageFont.truetype(FONT, 34)
d.text((24, 18), "SCORE 012340", font=f, fill=(255, 255, 255, 255))
d.text((24, 60), "LIVES x3", font=ImageFont.truetype(FONT, 22), fill=(255, 230, 120, 255))
img.save(f"{out}/test-game.png")

# 2. Title text on a transparent background, like the BRB / Intermission titles.
txt = Image.new("RGBA", (W, H), (0, 0, 0, 0))
d = ImageDraw.Draw(txt)
d.text((40, 110), "BE RIGHT BACK", font=ImageFont.truetype(FONT, 64), fill=(255, 255, 255, 255))
d.text((44, 200), "TAKING A SHORT BREAK", font=ImageFont.truetype(FONT, 32), fill=(255, 200, 80, 255))
txt.save(f"{out}/test-title.png")

# 3. Colourful "key art" used as the Ambient Background base image.
import math, random
art = Image.new("RGBA", (W, H))
px = art.load()
for y in range(H):
    for x in range(W):
        u, v = x / W, y / H
        px[x, y] = (int(40 + 200 * u), int(30 + 90 * abs(math.sin(u * 5 + v * 3))), int(220 - 170 * v), 255)
d = ImageDraw.Draw(art)
random.seed(4)
for _ in range(14):
    cx, cy, r = random.randint(0, W), random.randint(0, H), random.randint(15, 70)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=tuple(random.randint(60, 255) for _ in range(3)) + (255,))
d.text((150, 130), "KEY ART", font=ImageFont.truetype(FONT, 72), fill=(255, 255, 255, 255))
art.save(f"{out}/test-art.png")

# 4. A light "circuit board" texture with dark traces, for multiplying on top.
tex = Image.new("RGBA", (W, H), (235, 235, 235, 255))
d = ImageDraw.Draw(tex)
random.seed(7)
for _ in range(60):
    x, y = random.randrange(0, W, 20), random.randrange(0, H, 20)
    length = random.choice([40, 80, 120])
    if random.random() < 0.5:
        d.line([(x, y), (x + length, y)], fill=(60, 60, 60, 255), width=3)
    else:
        d.line([(x, y), (x, y + length)], fill=(60, 60, 60, 255), width=3)
    d.ellipse([x - 5, y - 5, x + 5, y + 5], outline=(40, 40, 40, 255), width=2)
tex.save(f"{out}/test-texture.png")

# 5. A 4:3 picture pillarboxed inside a 16:9 frame, as a full-screen emulator shows it.
bars = Image.new("RGBA", (W, H), (0, 0, 0, 255))
bars.paste(art.resize((480, 360)), (80, 0))
bars.save(f"{out}/test-pillarbox.png")
Image.new("RGBA", (W, H), (0, 0, 0, 255)).save(f"{out}/test-black.png")

# 6. A game with a black background: bright artwork that stops well short of the
#    edges, as on a title screen. This must NOT be taken for bars.
dark = Image.new("RGBA", (W, H), (0, 0, 0, 255))
dark.paste(art.resize((350, 250)), (145, 55))
dark.save(f"{out}/test-dark-game.png")
