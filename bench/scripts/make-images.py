"""Pictures for set A's test files, drawn with Pillow (installed with OCRmyPDF and img2pdf).

    python3 make-images.py <outdir>

Writes photo.jpg (RGB), photo-cmyk.jpg (CMYK), alpha.png (RGBA), photo.jp2 (JPEG 2000),
fax.tif (1-bit, CCITT group 4), scan-1.jpg..scan-3.jpg (grey pages of text, as a phone
scanner makes), srgb.icc (an sRGB profile made by Little CMS) and a 60-page large scan set
(big-01.jpg..big-60.jpg). Everything is drawn here, so it can be published with the files.
"""

import math
import os
import sys

from PIL import Image, ImageCms, ImageDraw, ImageFont

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

SANS = "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"
SERIF = "/usr/share/fonts/truetype/liberation2/LiberationSerif-Regular.ttf"


def photo(width, height):
    """A photo-like picture: smooth gradients, a sun and hills, with fine detail."""
    image = Image.new("RGB", (width, height))
    pixels = image.load()
    for y in range(height):
        for x in range(width):
            sky = y / height
            r = int(90 + 120 * sky + 20 * math.sin(x / 17.0))
            g = int(140 + 80 * sky + 15 * math.sin(y / 11.0))
            b = int(230 - 90 * sky)
            pixels[x, y] = (max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b)))
    draw = ImageDraw.Draw(image)
    draw.ellipse((width * 0.65, height * 0.1, width * 0.85, height * 0.37), fill=(255, 214, 90))
    for i, colour in enumerate([(46, 125, 50), (67, 160, 71), (27, 94, 32)]):
        top = height * (0.55 + 0.1 * i)
        points = [(0, height)] + [(x, top + 30 * math.sin(x / (60.0 + 20 * i) + i)) for x in range(0, width + 1, 8)] + [(width, height)]
        draw.polygon(points, fill=colour)
    return image


def text_page(lines, size=(1654, 2339), seed=0):
    """A grey page of text at 200 dpi, slightly uneven, as a phone scanner makes."""
    image = Image.new("L", size, 246)
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype(SERIF, 34)
    title = ImageFont.truetype(SANS, 52)
    draw.text((150, 160), lines[0], font=title, fill=30)
    y = 280
    for line in lines[1:]:
        draw.text((150, y), line, font=font, fill=40)
        y += 52
    # Paper grain and a shadow at the edge, deterministic.
    pixels = image.load()
    for y in range(0, size[1], 3):
        for x in range((y * 7 + seed) % 5, size[0], 5):
            pixels[x, y] = max(0, pixels[x, y] - ((x * 31 + y * 17 + seed) % 9))
    for x in range(40):
        for y in range(size[1]):
            pixels[x, y] = int(pixels[x, y] * (0.6 + x / 100))
    return image


LOREM = [
    "FynePDF bench: a scanned page",
    "This page was drawn as a picture of text, the way a phone",
    "scanner or an office copier makes a PDF. There is no text",
    "in the file, only an image of it.",
    "",
    "PDF/A converters must keep the picture as it is, declare",
    "what its colours mean, and not change how the page looks.",
    "",
] + [f"Line {n}: the quick brown fox jumps over the lazy dog." for n in range(1, 25)]

photo(800, 600).save(os.path.join(out, "photo.jpg"), quality=88)
photo(400, 300).convert("CMYK").save(os.path.join(out, "photo-cmyk.jpg"), quality=88)
photo(400, 300).save(os.path.join(out, "photo.jp2"), quality_mode="rates", quality_layers=[20])

alpha = Image.new("RGBA", (300, 300), (0, 0, 0, 0))
draw = ImageDraw.Draw(alpha)
for i in range(6):
    draw.ellipse((20 + i * 30, 20 + i * 20, 160 + i * 20, 160 + i * 20), fill=(220 - i * 30, 40 + i * 30, 120, 90 + i * 25))
alpha.save(os.path.join(out, "alpha.png"))

text_page(LOREM).convert("1").save(os.path.join(out, "fax.tif"), compression="group4", dpi=(200, 200))
for n in range(1, 4):
    lines = [f"FynePDF bench: scanned page {n} of 3"] + LOREM[1:]
    text_page(lines, seed=n).save(os.path.join(out, f"scan-{n}.jpg"), quality=75, dpi=(200, 200))
for n in range(1, 61):
    lines = [f"Large scan: page {n} of 60"] + LOREM[1:]
    text_page(lines, seed=n).save(os.path.join(out, f"big-{n:02d}.jpg"), quality=80, dpi=(200, 200))

with open(os.path.join(out, "srgb.icc"), "wb") as handle:
    handle.write(ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes())

print("images written to", out)
