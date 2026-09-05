#!/usr/bin/env python3
"""gen_icon.py — generate the cognitive-os-agent application icon (dist/cognitive-os-agent.ico).

Produces a small self-contained .ico (a 32x32 RGBA PNG entry) with a teal
rounded square and a white "core" bar. No image library required — the PNG is
written by hand with zlib. Used by package.sh before building the installer.
"""
import struct
import zlib

W = H = 32

def build_pixels():
    bg = (16, 124, 120, 255)     # teal
    core = (255, 255, 255, 255)  # white center bar
    px = []
    for y in range(H):
        row = []
        for x in range(W):
            # transparent corners
            if (x < 3 and y < 3) or (x >= W - 3 and y < 3) or \
               (x < 3 and y >= H - 3) or (x >= W - 3 and y >= H - 3):
                row.append((0, 0, 0, 0))
            else:
                row.append(bg)
        px.append(row)
    for y in range(10, 22):
        for x in range(12, 20):
            px[y][x] = core
    return px

def png_bytes(px):
    raw = b"".join(b"\x00" + bytes(c for p in row for c in p) for row in px)

    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data +
                struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)
    idat = zlib.compress(raw, 9)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", idat) + chunk(b"IEND", b""))

def main():
    import os
    out = os.path.join(os.path.dirname(__file__), "..", "dist", "cognitive-os-agent.ico")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    png = png_bytes(build_pixels())
    # ICO container with a single PNG entry (size stored as 0,0 = 256).
    header = struct.pack("<HHH", 0, 1, 1)
    entry = struct.pack("<BBBBHHII", 0, 0, 0, 0, 1, 32, len(png), 6 + 16)
    with open(out, "wb") as f:
        f.write(header + entry + png)
    print("wrote %s (%d bytes)" % (out, len(png) + 22))

if __name__ == "__main__":
    main()
