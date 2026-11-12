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
