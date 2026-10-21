"""Generates the hud-overlay demo's two pictures with the Python standard library only.

    python -I examples/demos/hud-overlay/references/make_images.py

- references/game-shot.png (1920x1080): a made-up game frame -- dusk sky with a bright sun, dark
  mountains and towers, a lit road -- with bright and dark areas next to each other under the
  crosshair, so the blend modes and the glass panels can be judged. It is the editor-only
  reference under the HUD; no real game screenshot is used.
- shared/minimap.png (256x256): a terrain map (grass checker, a river, roads) for the minimap.
"""

import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def write_png(path, width, height, rows):
    """`rows` yields one bytes object of width*3 RGB bytes per row; each row is Up-filtered."""
    raw = bytearray()
    previous = bytes(width * 3)
    for row in rows:
        raw.append(2)  # Up filter: smooth vertical gradients compress to almost nothing.
        raw.extend(bytes((a - b) & 0xFF for a, b in zip(row, previous)))
        previous = row

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def game_shot_rows(width, height):
    horizon = int(height * 0.58)
    sun = (1310, 430, 120)
    # Mountain ridge: a few summed sines, in pixels above the horizon.
    ridge = [int(150 + 70 * math.sin(x / 210.0) + 40 * math.sin(x / 67.0 + 1.3) + 18 * math.sin(x / 23.0))
             for x in range(width)]
    # Towers: (left, right, top) -- dark blocks; the one at 900..1010 sits under the crosshair.
    towers = [(160, 300, 330), (330, 410, 420), (900, 1010, 300), (1500, 1620, 360), (1650, 1700, 450)]
    for y in range(height):
        row = bytearray(width * 3)
        if y < horizon:
            t = y / horizon
            sky = lerp((18, 28, 70), (250, 150, 80), t ** 1.6)
        else:
            t = (y - horizon) / (height - horizon)
            ground = lerp((70, 60, 50), (22, 26, 20), t)
        for x in range(width):
            if y < horizon:
                c = sky
                dx, dy = x - sun[0], y - sun[1]
                d = math.sqrt(dx * dx + dy * dy)
                if d < sun[2]:
                    c = (255, 246, 214)
                elif d < sun[2] * 3:
                    k = 1.0 - (d - sun[2]) / (sun[2] * 2)
                    c = lerp(c, (255, 220, 160), k * k * 0.8)
                if y > horizon - ridge[x]:
                    c = lerp((40, 30, 60), (20, 18, 34), (y - (horizon - ridge[x])) / max(1, ridge[x]))
                for left, right, top in towers:
                    if left <= x < right and y >= top:
                        lit = ((x // 18) + (y // 26)) % 5 == 0 and (x % 18) < 9 and (y % 26) < 12
                        c = (255, 214, 120) if lit else (14, 14, 22)
            else:
                c = ground
                # A lit road narrowing to the horizon, with white lane marks.
                t = (y - horizon) / (height - horizon)
                half = 40 + 520 * t
                centre = 960
                if abs(x - centre) < half:
                    c = lerp((120, 120, 128), (200, 200, 205), t)
                    if abs(x - centre) < 6 + 10 * t and (int((y - horizon) / (8 + 40 * t)) % 2 == 0):
                        c = (250, 250, 250)
            o = x * 3
            row[o], row[o + 1], row[o + 2] = c
        yield bytes(row)


def minimap_rows(size):
    for y in range(size):
        row = bytearray(size * 3)
        for x in range(size):
            checker = ((x // 32) + (y // 32)) % 2
            c = (74, 120, 62) if checker else (92, 140, 70)
            # A river winding top to bottom.
            if abs(x - (128 + 40 * math.sin(y / 30.0))) < 9:
                c = (60, 120, 190)
            # Two roads.
            if abs(y - 180) < 4 or abs(x - 70) < 4:
                c = (205, 190, 150)
            # A few buildings.
            if (150 < x < 190 and 40 < y < 80) or (30 < x < 60 < y < 110):
                c = (150, 140, 135)
            o = x * 3
            row[o], row[o + 1], row[o + 2] = c
        yield bytes(row)


def main():
    write_png(os.path.join(HERE, "game-shot.png"), 1920, 1080, game_shot_rows(1920, 1080))
    write_png(os.path.join(ROOT, "shared", "minimap.png"), 256, 256, minimap_rows(256))


if __name__ == "__main__":
    main()
