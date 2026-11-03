#!/usr/bin/env python3
"""Generates the built-in sticker set in resources/stickers.

The whole set is drawn here from plain shapes, so every file is ours and no
third-party artwork, font or licence is involved. Running the script again
produces byte-identical files: the shapes are fixed, the sampler is fixed and
the PNG writer stores no timestamps.

The output is 1024 px square. A sticker is placed at about 5 cm, so that is
roughly 500 dpi on paper, and the canvas can show it sharp before it is
downscaled to a thumbnail.

Only the Python standard library is used, so the script runs anywhere the
project builds.

    python3 tools/make-sticker-assets.py
"""

import math
import os
import struct
import zlib
from array import array

SIZE = 1024         # output pixels per side, before the trim
SUPERSAMPLE = 2     # 2x2 samples per output pixel, antialiasing by coverage
GRID = SIZE * SUPERSAMPLE
KEYLINE = 3         # white die-cut edge, in output pixels
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

    rays = []
    points = [(0.5, 0.06), (0.545, 0.24), (0.455, 0.24)]
    for angle in (0, 45, 90, 135, 180, 225, 270, 315):
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

def bounds(predicate):
    """The output-pixel bounding box of one shape, by a coarse probe."""
    step = 8
    left = top = SIZE
    right = bottom = -1
    for gy in range(0, SIZE, step):
        for gx in range(0, SIZE, step):
            if predicate((gx + 0.5) / SIZE, (gy + 0.5) / SIZE):
                left, right = min(left, gx), max(right, gx)
                top, bottom = min(top, gy), max(bottom, gy)
    if right < left or bottom < top:
        return None
    return (max(0, left - step), max(0, top - step),
            min(SIZE - 1, right + step), min(SIZE - 1, bottom + step))


def dilate(row_masks, radius):
    """Grows a bit mask by `radius` pixels on both axes. Rows are ints, so the
    whole pass is a few thousand wide-integer operations instead of a nested
    pixel loop."""
    full = (1 << SIZE) - 1
    horizontal = []
    for mask in row_masks:
        grown = mask
        for shift in range(1, radius + 1):
            grown |= (mask << shift) | (mask >> shift)
        horizontal.append(grown & full)
    out = []
    count = len(horizontal)
    for index in range(count):
        grown = 0
        for shift in range(-radius, radius + 1):
            neighbour = index + shift
            if 0 <= neighbour < count:
                grown |= horizontal[neighbour]
        out.append(grown)
    return out


def render(layers, path):
    """One PNG. Every output pixel is the coverage-weighted average of the
    topmost layer under each of its sub-samples; a white keyline is drawn under
    the whole silhouette, and the finished image is trimmed to its content so
    the sticker's box is the artwork a person sees."""
    boxes = [bounds(predicate) for predicate, _ in layers]
    layer_pixels = [colour for _, colour in layers]
    total = SIZE * SIZE
    coverage = bytearray(total)
    colours = array('I', bytes(4 * total))
    first = 1.0 / GRID

    for py in range(SIZE):
        base = py * SIZE
        for px in range(SIZE):
            # Only the layers whose box contains this pixel can contribute.
            candidates = [index for index in range(len(layers))
                          if boxes[index] is not None
                          and boxes[index][0] <= px <= boxes[index][2]
                          and boxes[index][1] <= py <= boxes[index][3]]
            if not candidates:
                continue
            red = green = blue = hits = 0
            for sy in range(SUPERSAMPLE):
                y = (py * SUPERSAMPLE + sy + 0.5) * first
                for sx in range(SUPERSAMPLE):
                    x = (px * SUPERSAMPLE + sx + 0.5) * first
                    for index in reversed(candidates):
                        if layers[index][0](x, y):
                            colour = layer_pixels[index]
                            red += colour[0]
                            green += colour[1]
                            blue += colour[2]
                            hits += 1
                            break
            if hits:
                coverage[base + px] = hits
                colours[base + px] = (((red // hits) << 16)
                                      | ((green // hits) << 8) | (blue // hits))

    # The keyline follows the union silhouette, so a die-cut edge appears only
    # around the outside of the sticker, never between its own colours.
    masks = []
    for py in range(SIZE):
        base = py * SIZE
        mask = 0
        for px in range(SIZE):
            if coverage[base + px]:
                mask |= 1 << px
        masks.append(mask)
    grown = dilate(masks, KEYLINE)

    rows = []
    left = SIZE
    right = -1
    top = SIZE
    bottom = -1
    for py in range(SIZE):
        base = py * SIZE
        row = bytearray()
        line = grown[py]
        for px in range(SIZE):
            hits = coverage[base + px]
            if hits:
                colour = colours[base + px]
                alpha = int(round(255.0 * hits / (SUPERSAMPLE * SUPERSAMPLE)))
                row += bytes(((colour >> 16) & 0xff, (colour >> 8) & 0xff,
                              colour & 0xff, alpha))
            elif (line >> px) & 1:
                row += b"\xff\xff\xff\xff"
            else:
                row += b"\x00\x00\x00\x00"
                continue
            if px < left:
                left = px
            if px > right:
                right = px
            if py < top:
                top = py
            if py > bottom:
                bottom = py
        rows.append(row)

    if right < left or bottom < top:
        write_png(path, 1, 1, b"\x00\x00\x00\x00")
        return

    # Trim to the content, so a 6.5 cm sticker is 6.5 cm of artwork, not a
    # square with transparent padding around it.
    width = right - left + 1
    height = bottom - top + 1
    trimmed = b"".join(bytes(row[left * 4:(right + 1) * 4])
                       for row in rows[top:bottom + 1])
    write_png(path, width, height, trimmed)


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
