#!/usr/bin/env python3
"""Development-time generator: requires Pillow; its outputs are committed, and no build target, dev_check suite or
local_ci step runs it (Docs/design/56-symbol-identity-style-scope.md 56.5 item 2).

Generate shared/img/backdrop.png and backdrop-hud.png (1280x720 RGB).

An out-of-focus night scene with no objects and no text: a near-black to dark-grey vertical
gradient, two octaves of low-frequency value noise (distant light patches and dark masses), a blurred
horizon band at about 60 % height and a faint warm glow (top right on the title backdrop, far in
the centre on the HUD one). Fixed seeds: two runs give identical bytes.

Usage: python3 gen_backdrop.py [--out DIR]   (default: ../shared/img next to this script)
"""
import argparse, math, os, random
from PIL import Image, ImageFilter, ImageChops

W, H = 1280, 720
BG = (5, 5, 5)          # --color-bg
GREY = (44, 43, 41)     # dark grey near the cream ladder


def noise(rng, cells_x, cells_y):
    img = Image.new("L", (cells_x, cells_y))
    img.putdata([rng.randrange(256) for _ in range(cells_x * cells_y)])
    return img.resize((W, H), Image.BICUBIC)


def build(seed, glow_xy, glow_color, glow_radius, horizon_color):
    rng = random.Random(seed)
    # vertical gradient
    grad = Image.new("RGB", (1, H))
    px = []
    for y in range(H):
        t = y / (H - 1)
        k = 0.15 + 0.85 * (1 - t) ** 1.6
        px.append(tuple(int(BG[i] + (GREY[i] - BG[i]) * k * 0.55) for i in range(3)))
    grad.putdata(px)
    base = grad.resize((W, H))
    # two octaves of value noise -> light patches and dark masses
    n = Image.blend(noise(rng, 6, 4), noise(rng, 14, 9), 0.4).filter(ImageFilter.GaussianBlur(22))
    n = n.point(lambda v: int(max(0, v - 90) * 0.45))
    base = ImageChops.add(base, Image.merge("RGB", (n, n, n)))
    dark = noise(rng, 5, 3).filter(ImageFilter.GaussianBlur(40)).point(lambda v: int(v * 0.35))
    base = ImageChops.subtract(base, Image.merge("RGB", (dark, dark, dark)))
    # blurred horizon band at 60 %
    y0 = int(H * 0.6)
    band_px = Image.new("L", (1, H))
    band_px.putdata([int(120 * max(0.0, 1 - abs(y - y0) / 30) ** 2) for y in range(H)])
    band = band_px.resize((W, H)).filter(ImageFilter.GaussianBlur(14))
    wob = noise(rng, 9, 2).filter(ImageFilter.GaussianBlur(30))
    band = ImageChops.multiply(band, wob.point(lambda v: 90 + v * 165 // 255))
    base = ImageChops.subtract(base, Image.merge("RGB", (band, band, band)))
    base = ImageChops.add(base, Image.merge("RGB", tuple(band.point(lambda v, c=c: v * c // 255 // 5) for c in horizon_color)))
    # warm glow
    glow = Image.new("L", (W, H), 0)
    gx, gy = glow_xy
    gp = []
    for y in range(0, H, 4):
        for x in range(0, W, 4):
            d = math.hypot(x - gx, (y - gy) * 1.2) / glow_radius
            gp.append(int(255 * max(0.0, 1 - d) ** 2.2))
    g = Image.new("L", (W // 4, H // 4))
    g.putdata(gp)
    glow = g.resize((W, H), Image.BICUBIC).filter(ImageFilter.GaussianBlur(12))
    tint = Image.merge("RGB", tuple(glow.point(lambda v, c=c: v * c // 255) for c in glow_color))
    base = ImageChops.add(base, tint)
    return base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "shared", "img"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    build(2026, (1000, 150), (62, 46, 30), 520, (60, 52, 44)).save(
        os.path.join(a.out, "backdrop.png"), optimize=False, compress_level=9)
    build(2027, (640, 330), (52, 42, 30), 420, (50, 46, 42)).save(
        os.path.join(a.out, "backdrop-hud.png"), optimize=False, compress_level=9)


if __name__ == "__main__":
    main()
