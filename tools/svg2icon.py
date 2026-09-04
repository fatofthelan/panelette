#!/usr/bin/env python3
"""Rasterize a single SVG cubic-bezier path into a fill/stroke alpha-coverage
bitmap pair, hexdumped as a C PROGMEM byte array.

Same spirit as ttf2vlw.py (bake an anti-aliased asset into the firmware at
build time, colour it at draw time), but for the forecast page's cloud
shape instead of font glyphs. There's no cairosvg/Pillow available (and
pulling one in for a single one-time asset build isn't worth a new
dependency), and the source paths only ever use M/C/L/Z - so this is a
small purpose-built rasterizer rather than a general SVG renderer. Do not
point it at an arbitrary SVG; it will likely mishandle anything using arcs,
quadratic curves, or path commands other than M/C/L/Z.

Output is TWO 8-bit coverage layers (fill, stroke) rather than one
pre-composited colour bitmap - matching how the .vlw fonts store coverage
and let the caller pick the colour at draw time, so the same baked shape
can be reused at different colours/scales (the forecast page's "front" and
"back" cloud puffs are the same path, just a different fill colour and
scale).

Usage:
    ttf2vlw's venv already has what we need (stdlib only, actually):
    python3 tools/svg2icon.py > src/WeatherIcons.h
"""
import math
import re
import sys

# Source: the cloud-puff path shared by amCharts' free SVG weather icon
# pack (github.com/amcharts/weather, Apache License 2.0) across
# cloudy/cloudy-day/cloudy-night/rainy/snowy/thunder - see ASSETS.md.
CLOUD_PATH_D = (
    "M47.7,35.4c0-4.6-3.7-8.2-8.2-8.2c-1,0-1.9,0.2-2.8,0.5"
    "c-0.3-3.4-3.1-6.2-6.6-6.2c-3.7,0-6.7,3-6.7,6.7c0,0.8,0.2,1.6,0.4,2.3"
    "c-0.3-0.1-0.7-0.1-1-0.1c-3.7,0-6.7,3-6.7,6.7c0,3.6,2.9,6.6,6.5,6.7l17.2,0"
    "C44.2,43.3,47.7,39.8,47.7,35.4z"
)
# Net transform applied to the path in the source SVGs (outer translate(20,10)
# composed with the path's own translate(-20,-11)) - see tools/svg2icon.py's
# derivation notes in ASSETS.md. Net effect: translate(0,-1).
NET_DX, NET_DY = 0.0, -1.0

CANVAS = 64          # matches the source SVG's viewBox (0 0 64 64)
SUPERSAMPLE = 8       # coverage AA quality; also used as the dilate/erode unit
STROKE_HALF_PX = 0.6  # amCharts stroke-width is 1.2, centred on the path
BEZIER_STEPS = 24     # flatten quality per cubic segment


def parse_path(d):
    """Minimal M/C/L/Z path parser (absolute + relative). Returns a list of
    closed polygons, each a list of (x, y) tuples."""
    tokens = re.findall(r"[MmCcLlZz]|-?\d*\.?\d+(?:[eE]-?\d+)?", d)
    i = 0
    cur = (0.0, 0.0)
    start = (0.0, 0.0)
    cmd = None
    subpaths = []
    path = []

    def nextnum():
        nonlocal i
        v = float(tokens[i])
        i += 1
        return v

    while i < len(tokens):
        if tokens[i] in "MmCcLlZz":
            cmd = tokens[i]
            i += 1

        if cmd in ("M", "m"):
            x, y = nextnum(), nextnum()
            if cmd == "m":
                x += cur[0]; y += cur[1]
            cur = (x, y); start = cur
            if path:
                subpaths.append(path)
            path = [cur]
            cmd = "L" if cmd == "M" else "l"  # subsequent bare pairs are lineto
        elif cmd in ("L", "l"):
            x, y = nextnum(), nextnum()
            if cmd == "l":
                x += cur[0]; y += cur[1]
            cur = (x, y)
            path.append(cur)
        elif cmd in ("C", "c"):
            x1, y1, x2, y2, x, y = (nextnum() for _ in range(6))
            if cmd == "c":
                x1 += cur[0]; y1 += cur[1]
                x2 += cur[0]; y2 += cur[1]
                x += cur[0]; y += cur[1]
            for s in range(1, BEZIER_STEPS + 1):
                t = s / BEZIER_STEPS
                mt = 1 - t
                bx = mt**3 * cur[0] + 3 * mt * mt * t * x1 + 3 * mt * t * t * x2 + t**3 * x
                by = mt**3 * cur[1] + 3 * mt * mt * t * y1 + 3 * mt * t * t * y2 + t**3 * y
                path.append((bx, by))
            cur = (x, y)
        elif cmd in ("Z", "z"):
            path.append(start)
            subpaths.append(path)
            path = []
            cur = start
        else:
            raise ValueError(f"unsupported path command {cmd!r}")
    if path:
        subpaths.append(path)
    return subpaths


def rasterize_fill(polygon, w, h, ss):
    """Even-odd scanline fill at ss*ss supersample, returns a binary
    (0/1) grid at the supersampled resolution (w*ss x h*ss)."""
    sw, sh = w * ss, h * ss
    poly = [(x * ss, y * ss) for x, y in polygon]
    n = len(poly)
    grid = [[0] * sw for _ in range(sh)]
    for sy in range(sh):
        yc = sy + 0.5
        xs = []
        for k in range(n):
            x1, y1 = poly[k]
            x2, y2 = poly[(k + 1) % n]
            if y1 == y2:
                continue
            if (y1 <= yc < y2) or (y2 <= yc < y1):
                t = (yc - y1) / (y2 - y1)
                xs.append(x1 + t * (x2 - x1))
        xs.sort()
        for a in range(0, len(xs) - 1, 2):
            x0, x1 = xs[a], xs[a + 1]
            ix0 = max(0, int(math.floor(x0)))
            ix1 = min(sw - 1, int(math.ceil(x1)) - 1)
            row = grid[sy]
            for sx in range(ix0, ix1 + 1):
                row[sx] = 1
    return grid, sw, sh


def dilate(grid, w, h, r):
    if r <= 0:
        return [row[:] for row in grid]
    out = [[0] * w for _ in range(h)]
    for y in range(h):
        y0, y1 = max(0, y - r), min(h - 1, y + r)
        for x in range(w):
            x0, x1 = max(0, x - r), min(w - 1, x + r)
            hit = 0
            for yy in range(y0, y1 + 1):
                if any(grid[yy][x0:x1 + 1]):
                    hit = 1
                    break
            out[y][x] = hit
    return out


def erode(grid, w, h, r):
    if r <= 0:
        return [row[:] for row in grid]
    out = [[1] * w for _ in range(h)]
    for y in range(h):
        y0, y1 = max(0, y - r), min(h - 1, y + r)
        for x in range(w):
            x0, x1 = max(0, x - r), min(w - 1, x + r)
            keep = 1
            for yy in range(y0, y1 + 1):
                if not all(grid[yy][x0:x1 + 1]):
                    keep = 0
                    break
            out[y][x] = keep
    return out


def downsample(grid, w, h, ss):
    """Box-average an ss*ss-supersampled binary grid down to an 8-bit
    coverage byte per final pixel."""
    out = [[0] * w for _ in range(h)]
    area = ss * ss
    for y in range(h):
        for x in range(w):
            s = 0
            for dy in range(ss):
                row = grid[y * ss + dy]
                s += sum(row[x * ss:x * ss + ss])
            out[y][x] = round(255 * s / area)
    return out


def emit_array(name, w, h, rows):
    flat = [rows[y][x] for y in range(h) for x in range(w)]
    lines = [f"const uint16_t {name}_W = {w};", f"const uint16_t {name}_H = {h};",
             f"const uint8_t {name}[{w * h}] PROGMEM = {{"]
    for i in range(0, len(flat), 20):
        lines.append("  " + ",".join(str(v) for v in flat[i:i + 20]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    subpaths = parse_path(CLOUD_PATH_D)
    assert len(subpaths) == 1, "expected a single closed cloud-puff polygon"
    poly = [(x + NET_DX, y + NET_DY) for x, y in subpaths[0]]

    fill_ss, sw, sh = rasterize_fill(poly, CANVAS, CANVAS, SUPERSAMPLE)
    r = round(STROKE_HALF_PX * SUPERSAMPLE)
    ring_ss = [[1 if d and not e else 0
                for d, e in zip(drow, erow)]
               for drow, erow in zip(dilate(fill_ss, sw, sh, r), erode(fill_ss, sw, sh, r))]

    fill_cov = downsample(fill_ss, CANVAS, CANVAS, SUPERSAMPLE)
    stroke_cov = downsample(ring_ss, CANVAS, CANVAS, SUPERSAMPLE)

    out = []
    out.append("// Auto-generated by tools/svg2icon.py - do not hand-edit.")
    out.append("// Cloud-puff silhouette, traced from amCharts' free SVG weather icon")
    out.append("// pack (github.com/amcharts/weather, Apache License 2.0) - see ASSETS.md.")
    out.append("// Two 8-bit coverage layers (fill, stroke), colour applied at draw time -")
    out.append("// same idea as the .vlw font glyphs. Native 64x64; drawCloudPuff() in")
    out.append("// main.ino box-downsamples for any smaller runtime scale.")
    out.append("#pragma once")
    out.append("#include <Arduino.h>")
    out.append("")
    out.append(emit_array("CLOUD_PUFF_FILL", CANVAS, CANVAS, fill_cov))
    out.append("")
    out.append(emit_array("CLOUD_PUFF_STROKE", CANVAS, CANVAS, stroke_cov))
    out.append("")
    print("\n".join(out))


if __name__ == "__main__":
    main()
