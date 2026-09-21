#!/usr/bin/env python3
"""Generate Cognitive OS brain icons without third-party image libraries."""
import math
import os
import struct
import zlib


def put(px, x, y, color, alpha=1.0):
    if not (0 <= x < len(px[0]) and 0 <= y < len(px)):
        return
    old, a = px[y][x], max(0.0, min(1.0, alpha))
    px[y][x] = tuple(int(old[i] * (1 - a) + color[i] * a) for i in range(3)) + (255,)


def disc(px, cx, cy, radius, color):
    for y in range(max(0, int(cy - radius - 1)), min(len(px), int(cy + radius + 2))):
        for x in range(max(0, int(cx - radius - 1)), min(len(px[0]), int(cx + radius + 2))):
            d = math.hypot(x + .5 - cx, y + .5 - cy)
            if d < radius + .65:
                put(px, x, y, color, min(1.0, max(0.0, radius + .65 - d)))


def line(px, x0, y0, x1, y1, width, color):
    dx, dy = x1 - x0, y1 - y0
    length2 = dx * dx + dy * dy or 1
    for y in range(max(0, int(min(y0, y1) - width - 1)), min(len(px), int(max(y0, y1) + width + 2))):
        for x in range(max(0, int(min(x0, x1) - width - 1)), min(len(px[0]), int(max(x0, x1) + width + 2))):
            t = max(0.0, min(1.0, ((x + .5 - x0) * dx + (y + .5 - y0) * dy) / length2))
            d = math.hypot(x + .5 - (x0 + t * dx), y + .5 - (y0 + t * dy))
            if d < width + .65:
                put(px, x, y, color, min(1.0, max(0.0, width + .65 - d)))


def build_pixels(size):
    px = [[(0, 0, 0, 0) for _ in range(size)] for _ in range(size)]
    radius, inset = size * .22, size * .04
    for y in range(size):
        for x in range(size):
            dx = max(inset - x, 0, x - (size - inset - 1))
            dy = max(inset - y, 0, y - (size - inset - 1))
            if math.hypot(dx, dy) <= radius:
                t = (x + y) / max(1, 2 * (size - 1))
                px[y][x] = (int(29 + 72 * t), int(18 + 43 * t), int(79 + 162 * t), 255)

    s = size / 64.0
    brain, groove, glow = (242, 245, 255), (158, 134, 255), (112, 92, 245)
    for cx, cy, r in ((25, 24, 12), (20, 32, 12), (23, 42, 12),
                      (39, 24, 12), (44, 32, 12), (41, 42, 12)):
        disc(px, cx * s, cy * s, r * s, brain)
    line(px, 29 * s, 47 * s, 31 * s, 53 * s, 5 * s, brain)
    line(px, 35 * s, 47 * s, 33 * s, 53 * s, 5 * s, brain)
    line(px, 32 * s, 18 * s, 32 * s, 50 * s, 1.3 * s, groove)
    for coords in ((18, 28, 25, 28), (20, 37, 27, 37), (23, 22, 27, 24),
                   (46, 28, 39, 28), (44, 37, 37, 37), (41, 22, 37, 24)):
        line(px, *(v * s for v in coords), 1.25 * s, groove)
    disc(px, 32 * s, 16 * s, 2.2 * s, glow)
    return px


def png_bytes(px):
    h, w = len(px), len(px[0])
    raw = b"".join(b"\x00" + bytes(c for p in row for c in p) for row in px)

    def chunk(typ, data):
        return struct.pack(">I", len(data)) + typ + data + struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def write_ico(path, pngs):
    header, offset = struct.pack("<HHH", 0, 1, len(pngs)), 6 + 16 * len(pngs)
    entries, payload = [], []
    for size, data in pngs:
        dim = 0 if size >= 256 else size
        entries.append(struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset))
        payload.append(data)
        offset += len(data)
    with open(path, "wb") as out:
        out.write(header + b"".join(entries) + b"".join(payload))


def write_icns(path, png256, png512):
    chunks = b"ic08" + struct.pack(">I", len(png256) + 8) + png256
    chunks += b"ic09" + struct.pack(">I", len(png512) + 8) + png512
    with open(path, "wb") as out:
        out.write(b"icns" + struct.pack(">I", len(chunks) + 8) + chunks)


def main():
    dist = os.path.join(os.path.dirname(__file__), "..", "dist")
    os.makedirs(dist, exist_ok=True)
    pngs = [(size, png_bytes(build_pixels(size))) for size in (32, 64, 128, 256)]
    write_ico(os.path.join(dist, "cognitive-os-agent.ico"), pngs)
    write_icns(os.path.join(dist, "CognitiveOS.icns"), pngs[-1][1], png_bytes(build_pixels(512)))
    print("wrote brain icons to %s" % dist)


if __name__ == "__main__":
    main()
