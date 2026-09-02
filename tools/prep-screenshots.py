#!/usr/bin/env python3
"""Turn raw Panelette /screenshot.bmp captures into doc-ready PNGs.

  tools/prep-screenshots.py IN.bmp [IN.bmp ...] --out DIR [--scale N]
  tools/prep-screenshots.py --strip A.bmp B.bmp C.bmp --out FILE.png [--scale N]

Nearest-neighbour upscale (keeps the pixels crisp), pure stdlib - no Pillow.
Reads 24-bit uncompressed BMPs (what the device serves), writes PNG.
"""
import struct, zlib, sys, os, argparse


def read_bmp(path):
    d = open(path, "rb").read()
    if d[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    if bpp != 24:
        raise ValueError(f"{path}: expected 24bpp, got {bpp}")
    top_down = h < 0
    h = abs(h)
    row_pad = (-w * 3) % 4
    px = [[None] * w for _ in range(h)]
    p = off
    for ry in range(h):
        y = ry if top_down else h - 1 - ry
        for x in range(w):
            b, g, r = d[p], d[p + 1], d[p + 2]
            px[y][x] = (r, g, b)
            p += 3
        p += row_pad
    return w, h, px


def scale(px, w, h, n):
    if n == 1:
        return w, h, px
    out = [[px[y // n][x // n] for x in range(w * n)] for y in range(h * n)]
    return w * n, h * n, out


def write_png(path, w, h, px):
    raw = bytearray()
    for row in px:
        raw.append(0)  # filter: none
        for (r, g, b) in row:
            raw += bytes((r, g, b))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def hcat(imgs, gap, bg):
    h = max(im[1] for im in imgs)
    w = sum(im[0] for im in imgs) + gap * (len(imgs) - 1)
    out = [[bg] * w for _ in range(h)]
    x0 = 0
    for (iw, ih, ipx) in imgs:
        oy = (h - ih) // 2
        for y in range(ih):
            out[oy + y][x0:x0 + iw] = ipx[y]
        x0 += iw + gap
    return w, h, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--strip", action="store_true", help="concat inputs into one PNG (--out is the file)")
    ap.add_argument("--gap", type=int, default=24)
    a = ap.parse_args()

    imgs = []
    for p in a.inputs:
        w, h, px = read_bmp(p)
        imgs.append(scale(px, w, h, a.scale))

    if a.strip:
        w, h, px = hcat(imgs, a.gap, (13, 17, 23))
        write_png(a.out, w, h, px)
        print(f"wrote {a.out}  ({w}x{h})")
        return

    os.makedirs(a.out, exist_ok=True)
    for src, (w, h, px) in zip(a.inputs, imgs):
        name = os.path.splitext(os.path.basename(src))[0] + ".png"
        dst = os.path.join(a.out, name)
        write_png(dst, w, h, px)
        print(f"wrote {dst}  ({w}x{h})")


if __name__ == "__main__":
    main()
