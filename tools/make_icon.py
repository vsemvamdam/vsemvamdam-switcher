# Generates res/app.ico — an original mark: dark squircle with two overlapping "account" discs.
from PIL import Image, ImageDraw
import sys

def squircle_mask(size, n=5.0):
    m = Image.new("L", (size, size), 0)
    px = m.load()
    r = size / 2.0
    for y in range(size):
        for x in range(size):
            # supersample 4x for smooth edges
            cov = 0
            for sy in range(4):
                for sx in range(4):
                    dx = abs((x + (sx + 0.5) / 4) - r) / r
                    dy = abs((y + (sy + 0.5) / 4) - r) / r
                    if dx ** n + dy ** n <= 1.0:
                        cov += 1
            px[x, y] = int(255 * cov / 16)
    return m

def render(size):
    S = size * 4
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    bg = Image.new("RGBA", (S, S), (22, 22, 24, 255))
    mask = squircle_mask(size).resize((S, S), Image.LANCZOS)
    img.paste(bg, (0, 0), mask)
    d = ImageDraw.Draw(img)
    # back disc: warm sand ring
    cx1, cy1, rad = S * 0.40, S * 0.44, S * 0.205
    w = max(4, int(S * 0.055))
    d.ellipse([cx1 - rad, cy1 - rad, cx1 + rad, cy1 + rad], outline=(214, 196, 168, 255), width=w)
    # front disc: solid off-white
    cx2, cy2 = S * 0.60, S * 0.58
    d.ellipse([cx2 - rad, cy2 - rad, cx2 + rad, cy2 + rad], fill=(246, 243, 238, 255))
    return img.resize((size, size), Image.LANCZOS)

sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
imgs = [render(s) for s in sizes]
imgs[-1].save(sys.argv[1], sizes=[(s, s) for s in sizes], append_images=imgs[:-1])
imgs[-1].save(sys.argv[2])
