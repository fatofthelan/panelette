#!/usr/bin/env python3
"""Minimal TTF -> TFT_eSPI .vlw converter (v11 format), ASCII 0x20-0x7E + degree.
Usage: ttf2vlw.py font.ttf out.vlw <px_size> [wght]
"""
import sys, struct, freetype

def be(v): return struct.pack('>i', v)

def main():
    ttf, out, px = sys.argv[1], sys.argv[2], int(sys.argv[3])
    wght = float(sys.argv[4]) if len(sys.argv) > 4 else 400.0

    face = freetype.Face(ttf)
    # set the variable-font weight axis (Noto Sans axes are wght, wdth)
    try:
        import ctypes
        FT_Set_Var_Design_Coordinates = freetype.FT_Library_filename  # noqa (probe)
    except Exception:
        pass
    try:
        vinfo = face.get_variation_info()
        coords = []
        for ax in vinfo.axes:
            nm = ax.name.decode() if isinstance(ax.name, bytes) else ax.name
            if 'Weight' in nm or nm == 'wght':
                coords.append(int(wght))
            elif 'Width' in nm or nm == 'wdth':
                coords.append(100)
            else:
                coords.append(int(ax.default))
        face.set_var_design_coords(coords)
    except Exception as e:
        print('  (weight axis not set:', e, ')')
    face.set_pixel_sizes(0, px)

    codes = list(range(0x20, 0x7F)) + [0xB0]  # printable ASCII + degree sign
    glyphs, bitmaps = [], []
    ascent = descent = 0
    for cp in codes:
        face.load_char(chr(cp), freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
        g = face.glyph
        bm = g.bitmap
        w, h = bm.width, bm.rows
        buf = bytes()
        for r in range(h):
            row = bm.buffer[r * bm.pitch : r * bm.pitch + w]
            buf += bytes(row)
        glyphs.append((cp, h, w, g.advance.x >> 6, g.bitmap_top, g.bitmap_left))
        bitmaps.append(buf)
        # ascent/descent from the actual ASCII glyphs (like Processing does)
        if 0x21 <= cp <= 0x7E:
            ascent = max(ascent, g.bitmap_top)
            descent = max(descent, h - g.bitmap_top)

    with open(out, 'wb') as f:
        f.write(be(len(glyphs)) + be(11) + be(px) + be(0) + be(ascent) + be(descent))
        for (cp, h, w, adv, dy, dx) in glyphs:
            f.write(be(cp) + be(h) + be(w) + be(adv) + be(dy) + be(dx) + be(0))
        for b in bitmaps:
            f.write(b)

    total = 24 + len(glyphs) * 28 + sum(len(b) for b in bitmaps)
    print(f"{out}: {len(glyphs)} glyphs, {px}px, ascent {ascent} descent {descent}, {total} bytes")

if __name__ == '__main__':
    main()
