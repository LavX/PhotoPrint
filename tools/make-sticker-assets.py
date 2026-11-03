#!/usr/bin/env python3
"""Generates the built-in sticker set in resources/stickers.

The whole set is drawn here from plain shapes, so every file is ours and no
third-party artwork, font or licence is involved. Running the script again
produces byte-identical files: the shapes are fixed, the sampler is fixed and
the PNG writer stores no timestamps.

Only the Python standard library is used, so the script runs anywhere the
project builds.

    python3 tools/make-sticker-assets.py
"""

import math
import os
import struct
import zlib

SIZE = 256          # output pixels per side
SUPERSAMPLE = 4     # 4x4 samples per output pixel, antialiasing by coverage
GRID = SIZE * SUPERSAMPLE
OUT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "resources", "stickers")


# --- small vector helpers ---------------------------------------------------

def rotate(x, y, cx, cy, degrees):
    radians = math.radians(degrees)
    dx, dy = x - cx, y - cy
    return (cx + dx * math.cos(radians) - dy * math.sin(radians),
            cy + dx * math.sin(radians) + dy * math.cos(radians))


def polygon(points):
    """Even-odd point-in-polygon test for a closed list of (x, y)."""
    def test(x, y):
        inside = False
        count = len(points)
        for index in range(count):
            x1, y1 = points[index]
            x2, y2 = points[(index + 1) % count]
            if (y1 > y) != (y2 > y):
                cross = x1 + (y - y1) * (x2 - x1) / (y2 - y1)
                if x < cross:
                    inside = not inside
        return inside
    return test


def ellipse(cx, cy, rx, ry, angle=0.0):
    def test(x, y):
        lx, ly = rotate(x, y, cx, cy, -angle)
        dx, dy = (lx - cx) / rx, (ly - cy) / ry
        return dx * dx + dy * dy <= 1.0
    return test


def circle(cx, cy, radius):
    return ellipse(cx, cy, radius, radius)


def ring(cx, cy, outer, inner):
    outside = circle(cx, cy, outer)
    hole = circle(cx, cy, inner)

    def test(x, y):
        return outside(x, y) and not hole(x, y)
    return test


def rounded_rect(x0, y0, x1, y1, radius):
    def test(x, y):
        if x < x0 or x > x1 or y < y0 or y > y1:
            return False
        nearest_x = min(max(x, x0 + radius), x1 - radius)
        nearest_y = min(max(y, y0 + radius), y1 - radius)
        dx, dy = x - nearest_x, y - nearest_y
        return dx * dx + dy * dy <= radius * radius
    return test


def star(cx, cy, outer, inner, points=5, rotation=-90.0):
    vertices = []
    for index in range(points * 2):
        radius = outer if index % 2 == 0 else inner
        angle = math.radians(rotation + index * 180.0 / points)
        vertices.append((cx + radius * math.cos(angle), cy + radius * math.sin(angle)))
    return polygon(vertices)


def heart(cx, cy, scale):
    def test(x, y):
        # The classic implicit heart, scaled into the unit square.
        u = (x - cx) / scale
        v = -(y - cy) / scale + 0.18
        value = (u * u + v * v - 1.0) ** 3 - u * u * v * v * v
        return value <= 0.0
    return test


def union(*tests):
    def test(x, y):
        return any(part(x, y) for part in tests)
    return test


def difference(whole, hole):
    def test(x, y):
        return whole(x, y) and not hole(x, y)
    return test


def any_of(*layers):
    def test(x, y):
        return any(predicate(x, y) for predicate, _ in layers)
    return test


# --- the set ---------------------------------------------------------------

def shape_layers():
    """alias -> list of (predicate, (r, g, b)) painted bottom to top."""
    layers = {}

    layers["star"] = [
        (star(0.5, 0.52, 0.42, 0.18), (245, 176, 45)),
        (star(0.5, 0.52, 0.27, 0.115), (250, 205, 92)),
    ]

    layers["heart"] = [
        (heart(0.5, 0.46, 0.30), (222, 60, 78)),
        (ellipse(0.42, 0.38, 0.06, 0.045, -25), (255, 148, 158)),
    ]

    # A right-pointing arrow with a tail.
    layers["arrow"] = [
        (union(polygon([(0.16, 0.42), (0.58, 0.42), (0.58, 0.28), (0.86, 0.5),
                        (0.58, 0.72), (0.58, 0.58), (0.16, 0.58)]),
               ellipse(0.20, 0.5, 0.06, 0.08)),
         (46, 134, 171)),
    ]

    # A speech bubble with a tail on the lower left.
    layers["speech"] = [
        (rounded_rect(0.12, 0.16, 0.88, 0.66, 0.16), (59, 178, 115)),
        (polygon([(0.28, 0.60), (0.46, 0.62), (0.26, 0.86)]), (59, 178, 115)),
        (ellipse(0.34, 0.410, 0.035, 0.035), (255, 255, 255)),
        (ellipse(0.50, 0.410, 0.035, 0.035), (255, 255, 255)),
        (ellipse(0.66, 0.410, 0.035, 0.035), (255, 255, 255)),
    ]

    rays = [(polygon([(0.5, 0.06), (0.545, 0.24), (0.455, 0.24)]), (242, 153, 46))]
    for angle in (45, 90, 135, 180, 225, 270, 315):
        points = [(0.5, 0.06), (0.545, 0.24), (0.455, 0.24)]
        rays.append((polygon([rotate(x, y, 0.5, 0.5, angle) for x, y in points]),
                     (242, 153, 46)))
    layers["sun"] = rays + [
        (circle(0.5, 0.5, 0.20), (250, 200, 60)),
        (circle(0.5, 0.5, 0.13), (255, 226, 122)),
    ]

    petals = []
    for angle in range(0, 360, 60):
        cx, cy = rotate(0.5, 0.29, 0.5, 0.5, angle)
        petals.append((ellipse(cx, cy, 0.14, 0.21, angle), (233, 106, 154)))
    layers["flower"] = petals + [
        (circle(0.5, 0.5, 0.115), (250, 202, 66)),
        (circle(0.5, 0.5, 0.055), (255, 230, 140)),
    ]

    layers["confetti"] = [
        (ellipse(0.22, 0.26, 0.07, 0.035, -30), (228, 84, 140)),
        (ellipse(0.52, 0.16, 0.065, 0.032, 20), (59, 178, 115)),
        (ellipse(0.78, 0.32, 0.07, 0.033, -55), (46, 134, 171)),
        (ellipse(0.34, 0.56, 0.065, 0.032, 45), (245, 176, 45)),
        (ellipse(0.66, 0.62, 0.07, 0.035, -15), (233, 106, 154)),
        (ellipse(0.16, 0.80, 0.06, 0.030, 10), (46, 134, 171)),
        (ellipse(0.50, 0.86, 0.065, 0.032, -40), (59, 178, 115)),
        (ellipse(0.84, 0.78, 0.06, 0.030, 30), (245, 176, 45)),
    ]

    # A hollow frame with a soft double line.
    layers["frame"] = [
        (difference(rounded_rect(0.10, 0.10, 0.90, 0.90, 0.07),
                    rounded_rect(0.18, 0.18, 0.82, 0.82, 0.05)), (201, 158, 84)),
        (difference(rounded_rect(0.235, 0.235, 0.765, 0.765, 0.04),
                    rounded_rect(0.275, 0.275, 0.725, 0.725, 0.03)), (232, 200, 137)),
    ]

    layers["ring"] = [
        (ring(0.5, 0.5, 0.38, 0.28), (142, 93, 184)),
        (ring(0.5, 0.5, 0.38, 0.34), (176, 138, 210)),
    ]

    # A four-point sparkle with a smaller companion.
    sparkle = polygon([(0.5, 0.10), (0.565, 0.435), (0.90, 0.5), (0.565, 0.565),
                       (0.5, 0.90), (0.435, 0.565), (0.10, 0.5), (0.435, 0.435)])
    companion = polygon([(0.76, 0.68), (0.795, 0.795), (0.91, 0.83), (0.795, 0.865),
                         (0.76, 0.98), (0.725, 0.865), (0.61, 0.83), (0.725, 0.795)])
    layers["sparkle"] = [
        (sparkle, (150, 110, 214)),
        (companion, (196, 168, 232)),
    ]

    # A diagonal ribbon with two tails.
    layers["ribbon"] = [
        (polygon([(0.14, 0.20), (0.86, 0.20), (0.86, 0.44), (0.14, 0.44)]), (46, 134, 171)),
        (polygon([(0.14, 0.44), (0.30, 0.44), (0.30, 0.86), (0.14, 0.72)]), (36, 104, 133)),
        (polygon([(0.70, 0.44), (0.86, 0.44), (0.86, 0.72), (0.70, 0.86)]), (36, 104, 133)),
        (ellipse(0.42, 0.32, 0.055, 0.055), (255, 255, 255)),
        (ellipse(0.58, 0.32, 0.055, 0.055), (255, 255, 255)),
    ]

    # A rosette badge: a many-pointed burst with a round centre.
    layers["badge"] = [
        (star(0.5, 0.52, 0.44, 0.33, points=16, rotation=-90.0), (222, 60, 78)),
        (circle(0.5, 0.52, 0.235), (250, 236, 214)),
        (ring(0.5, 0.52, 0.175, 0.145), (222, 60, 78)),
    ]

    return layers


# --- rasteriser ------------------------------------------------------------

def render(layers, path):
    """One PNG: every output pixel is the coverage-weighted average of the
    topmost layer under each of its sub-samples."""
    # A coarse scan finds the extent of everything, so the empty margin costs
    # almost nothing.
    left = top = GRID
    right = bottom = 0
    coarse = 8
    for gy in range(0, GRID, coarse):
        for gx in range(0, GRID, coarse):
            x = (gx + 0.5) / GRID
            y = (gy + 0.5) / GRID
            if any(predicate(x, y) for predicate, _ in layers):
                left, right = min(left, gx), max(right, gx)
                top, bottom = min(top, gy), max(bottom, gy)
    if right <= left or bottom <= top:
        left, top, right, bottom = 0, 0, GRID - 1, GRID - 1
    left = max(0, left - coarse)
    top = max(0, top - coarse)
    right = min(GRID - 1, right + coarse)
    bottom = min(GRID - 1, bottom + coarse)

    rows = []
    step = 1.0 / GRID
    for py in range(SIZE):
        row = bytearray()
        for px in range(SIZE):
            red = green = blue = hit = 0
            for sy in range(SUPERSAMPLE):
                y = ((py * SUPERSAMPLE + sy) + 0.5) * step
                for sx in range(SUPERSAMPLE):
                    x = ((px * SUPERSAMPLE + sx) + 0.5) * step
                    if not (left * step <= x <= (right + 1) * step
                            and top * step <= y <= (bottom + 1) * step):
                        continue
                    for predicate, colour in reversed(layers):
                        if predicate(x, y):
                            red += colour[0]
                            green += colour[1]
                            blue += colour[2]
                            hit += 1
                            break
            if hit == 0:
                row += b"\x00\x00\x00\x00"
            else:
                alpha = int(round(255.0 * hit / (SUPERSAMPLE * SUPERSAMPLE)))
                row += bytes((red // hit, green // hit, blue // hit, alpha))
        rows.append(bytes(row))

    write_png(path, SIZE, SIZE, b"".join(rows))


def write_png(path, width, height, rgba):
    def chunk(tag, payload):
        body = tag + payload
        return (struct.pack(">I", len(payload)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter: none, so the output is deterministic
        raw += rgba[y * stride:(y + 1) * stride]

    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    data += chunk(b"IEND", b"")
    with open(path, "wb") as handle:
        handle.write(data)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    for alias, layers in sorted(shape_layers().items()):
        path = os.path.join(OUT_DIR, alias + ".png")
        render(layers, path)
        print("wrote", os.path.relpath(path))


if __name__ == "__main__":
    main()
