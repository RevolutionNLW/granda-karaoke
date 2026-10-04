#!/usr/bin/env python3
"""Builds the application icons from the approved artwork (macOS only: sips, iconutil).

    python3 packaging/icon/make_icons.py

app-icon-supplied.png  the approved artwork exactly as supplied (1254 x 1254, no
                       transparency: the area outside the rounded tile is black)
app-icon-master.png    the same artwork with only that outside area transparent;
                       every pixel inside the tile is unchanged
FrankiesKaraokeStudio.icns / .ico
                       every size made directly from the master (never from
                       another, smaller size)
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SUPPLIED = os.path.join(HERE, "app-icon-supplied.png")
MASTER = os.path.join(HERE, "app-icon-master.png")
ICNS = os.path.join(HERE, "FrankiesKaraokeStudio.icns")
ICO = os.path.join(HERE, "FrankiesKaraokeStudio.ico")

# macOS: name -> pixels (iconutil's iconset).
ICONSET = {
    "icon_16x16.png": 16, "icon_16x16@2x.png": 32,
    "icon_32x32.png": 32, "icon_32x32@2x.png": 64,
    "icon_128x128.png": 128, "icon_128x128@2x.png": 256,
    "icon_256x256.png": 256, "icon_256x256@2x.png": 512,
    "icon_512x512.png": 512, "icon_512x512@2x.png": 1024,
}
# Windows: the standard sizes, plus 20 and 40 (16 and 32 at 125% scale).
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

# Outside the tile the supplied image is black with faint compression noise.
OUTSIDE_MAX = 6


def read_png(path):
    """8-bit RGB or RGBA, non-interlaced PNG -> (width, height, channels, rows)."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, header = 8, b"", None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    width, height, depth, colour, _, _, interlace = header
    assert depth == 8 and colour in (2, 6) and interlace == 0, (path, header)
    channels = 4 if colour == 6 else 3
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, previous, i = [], bytearray(stride), 0
    for _ in range(height):
        kind = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = previous[x]
            c = previous[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        previous = line
    return width, height, channels, rows


def write_png(path, width, height, channels, rows):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    header = struct.pack(">IIBBBBB", width, height, 8, 6 if channels == 4 else 2, 0, 0, 0)
    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
                  + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def make_master():
    """The outside area (near-black, joined to the image's edge) becomes
    transparent; the 1-pixel anti-aliased rim of the tile gets the soft edge
    it had before it was flattened onto black. Nothing else changes."""
    width, height, channels, rows = read_png(SUPPLIED)
    assert channels == 3, "the supplied artwork has no transparency"
    pixel = lambda x, y: rows[y][x * 3:x * 3 + 3]
    outside = [[False] * width for _ in range(height)]
    stack = [(x, y) for x in range(width) for y in (0, height - 1)]
    stack += [(x, y) for y in range(height) for x in (0, width - 1)]
    while stack:
        x, y = stack.pop()
        if outside[y][x] or max(pixel(x, y)) > OUTSIDE_MAX:
            continue
        outside[y][x] = True
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < width and 0 <= ny < height and not outside[ny][nx]:
                stack.append((nx, ny))
    neighbours = [(dx, dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1) if dx or dy]
    rim = lambda x, y: any(0 <= x + dx < width and 0 <= y + dy < height and outside[y + dy][x + dx]
                           for dx, dy in neighbours)
    out = []
    for y in range(height):
        line = bytearray()
        for x in range(width):
            r, g, b = pixel(x, y)
            alpha = 255
            if outside[y][x]:
                r = g = b = alpha = 0
            elif rim(x, y):
                # Its colour came from the tile over black: alpha is how much
                # tile it holds, judged against the tile just inside it.
                inner = [pixel(x + dx, y + dy) for dx, dy in neighbours
                         if 0 <= x + dx < width and 0 <= y + dy < height
                         and not outside[y + dy][x + dx] and not rim(x + dx, y + dy)]
                if inner:
                    reference = [sum(p[i] for p in inner) / len(inner) for i in range(3)]
                    level = max(reference)
                    if level > 0 and max(r, g, b) < level:
                        alpha = max(1, round(255 * max(r, g, b) / level))
                        r, g, b = (min(255, round(c * 255 / alpha)) for c in (r, g, b))
            line += bytes((r, g, b, alpha))
        out.append(line)
    write_png(MASTER, width, height, 4, out)
    print("master: %s (%dx%d, %d outside pixels transparent)"
          % (MASTER, width, height, sum(map(sum, outside))))


def resized(size, folder):
    """One size, made from the master itself."""
    path = os.path.join(folder, "%d.png" % size)
    if not os.path.exists(path):
        subprocess.run(["sips", "-s", "format", "png", "-z", str(size), str(size), MASTER, "--out", path],
                       check=True, stdout=subprocess.DEVNULL)
        width, height, channels, _ = read_png(path)
        assert (width, height, channels) == (size, size, 4), (path, width, height, channels)
    return path


def bmp_entry(png_path):
    """A 32-bit DIB icon image (with its AND mask), as Windows draws small icons best."""
    width, height, channels, rows = read_png(png_path)
    pixels = b"".join(bytes((row[x * 4 + 2], row[x * 4 + 1], row[x * 4], row[x * 4 + 3]))
                      for row in reversed(rows) for x in range(width))
    mask_stride = ((width + 31) // 32) * 4
    mask = bytearray()
    for row in reversed(rows):
        bits = bytearray(mask_stride)
        for x in range(width):
            if row[x * 4 + 3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    header = struct.pack("<IiiHHIIiiII", 40, width, height * 2, 1, 32, 0,
                         len(pixels) + len(mask), 0, 0, 0, 0)
    return header + pixels + bytes(mask)


def main():
    if sys.platform != "darwin":
        sys.exit("make_icons.py needs macOS (sips, iconutil)")
    make_master()
    folder = tempfile.mkdtemp(prefix="fks-icons-")
    try:
        iconset = os.path.join(folder, "FrankiesKaraokeStudio.iconset")
        os.mkdir(iconset)
        for name, size in ICONSET.items():
            shutil.copyfile(resized(size, folder), os.path.join(iconset, name))
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", ICNS], check=True)
        print("icns:", ICNS, sorted(set(ICONSET.values())))

        images = []
        for size in ICO_SIZES:
            path = resized(size, folder)
            images.append(open(path, "rb").read() if size >= 256 else bmp_entry(path))
        offset = 6 + 16 * len(images)
        directory = b""
        for size, image in zip(ICO_SIZES, images):
            side = 0 if size >= 256 else size
            directory += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(image), offset)
            offset += len(image)
        with open(ICO, "wb") as out:
            out.write(struct.pack("<HHH", 0, 1, len(images)) + directory + b"".join(images))
        print("ico:", ICO, ICO_SIZES)
    finally:
        shutil.rmtree(folder)


if __name__ == "__main__":
    main()
