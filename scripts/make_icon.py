"""Generates res/snow.ico (16/24/32/48/64/256 px) without third-party packages.

The same procedural snowflake as the tray icon (src/app/app.cpp MakeIcon).
Run: python scripts/make_icon.py
"""
import math
import os
import struct
import zlib


def flake(size):
    c = size / 2.0
    R = size * 0.46
    w = max(1.0, size / 16.0)
    segs = []
    for k in range(6):
        a = k * math.pi / 3
        dx, dy = math.cos(a), math.sin(a)
        segs.append((c, c, c + dx * R, c + dy * R))
        for t in (0.5, 0.75):
            bx, by = c + dx * R * t, c + dy * R * t
            bl = R * 0.28 * (1.1 - t * 0.4)
            for s in (-1, 1):
                ba = a + s * math.pi / 4
                segs.append((bx, by, bx + math.cos(ba) * bl, by + math.sin(ba) * bl))
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            px, py = x + 0.5, y + 0.5
            dmin = 1e9
            for x0, y0, x1, y1 in segs:
                vx, vy = x1 - x0, y1 - y0
                t = ((px - x0) * vx + (py - y0) * vy) / (vx * vx + vy * vy)
                t = min(1.0, max(0.0, t))
                ex, ey = x0 + vx * t - px, y0 + vy * t - py
                dmin = min(dmin, math.hypot(ex, ey))
            a = min(1.0, max(0.0, w * 0.5 + 0.5 - dmin))
            core = min(1.0, max(0.0, w * 0.5 - dmin))
            r, g, b = 0.35 + 0.65 * core, 0.55 + 0.45 * core, 0.85 + 0.15 * core
            row += bytes((int(r * 255), int(g * 255), int(b * 255), int(a * 255)))  # RGBA, straight alpha
        rows.append(bytes(row))
    return rows


def png(size, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main():
    sizes = [16, 24, 32, 48, 64, 256]
    images = [png(s, flake(s)) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, data in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out = os.path.join(os.path.dirname(__file__), "..", "res", "snow.ico")
    with open(out, "wb") as f:
        f.write(header + entries + b"".join(images))
    print("wrote", os.path.normpath(out))


if __name__ == "__main__":
    main()
