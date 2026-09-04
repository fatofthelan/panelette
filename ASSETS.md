# Assets

Non-font embedded assets, same idea as [FONTS.md](FONTS.md): what's bundled,
where it came from, and what license it's under.

## Forecast page cloud icon

`src/WeatherIcons.h` bakes a single cloud-puff silhouette (two 8-bit
coverage layers - fill, stroke - colored at draw time) traced from the
free SVG weather icon pack published by amCharts:

| Bundled file | Source | Copyright | License |
|---|---|---|---|
| `WeatherIcons.h` (`CLOUD_PUFF_FILL`/`CLOUD_PUFF_STROKE`) | amCharts free SVG weather icons | © amCharts | Apache License 2.0 |

Upstream: https://github.com/amcharts/weather (the specific path traced is
the cloud shape shared by `cloudy.svg` / `cloudy-day-*.svg` /
`cloudy-night-*.svg` / `rainy-*.svg` / `snowy-*.svg` / `thunder.svg` in
`assets/img/weather/`). amCharts also publishes the same pack from
https://www.amcharts.com/free-animated-svg-weather-icons/ under CC BY 4.0;
the GitHub repo's own `LICENSE` file (Apache 2.0) is what's cited here
since it's directly verifiable against the source used.

The full text of the Apache License 2.0 is in
[`licenses/Apache-2.0.txt`](licenses/Apache-2.0.txt).

### How it was traced

`tools/svg2icon.py` is a small purpose-built rasterizer (stdlib only, no
cairosvg/Pillow) that parses one specific cubic-bezier path (`M`/`C`/`L`/`Z`
commands only - it will mishandle an arbitrary SVG using arcs or quadratic
curves) and rasterizes it at 8x supersampling into an anti-aliased 8-bit
coverage grid, then derives a stroke ring via binary dilate/erode of that
same grid. Output is two PROGMEM byte arrays at the path's native 64x64
size - `drawCloudPuff()` in `main.ino` box-downsamples them to whatever
size a call site needs and composites fill-then-stroke by reading the real
pixel underneath and blending toward it (`blendColor565()`), the same
"sample the actual pixel, don't guess a background colour" approach used
for the rest of the file's anti-aliased primitives (see CLAUDE.md's
"Anti-aliased primitives" section).

To regenerate after editing the traced path or supersample/stroke
constants in `tools/svg2icon.py`:

```
python3 tools/svg2icon.py > src/WeatherIcons.h
```

### Why a bitmap and not more vector primitives

The forecast page's sun/moon/rain/snow accents are still drawn
procedurally (circles, lines) - those never had a shape problem. Only the
cloud silhouette itself needed a bitmap: three overlapping circles never
quite reads as a real cloud, no matter how the AA on their seams is fixed.
Tracing amCharts' actual cloud path gets the proportions right in a way
that's impractical to hand-tune from primitives.

The thunderbolt is the one exception drawn from traced amCharts geometry
without a bitmap: `thunder.svg`'s bolt is a 7-point polygon
(`points="14.3,-2.9 20.5,-2.9 16.4,4.3 20.3,4.3 11.5,14.6 14.9,6.9
11.1,6.9"`, in the same 64-unit canvas frame as the cloud path once its
own `translate(-9,28), scale(1.2)` is composed with the shared outer
group translate). TFT_eSPI has no general filled-polygon primitive, so
this was ear-clipped once, offline, into 5 triangles (a one-off Python
snippet, not part of `svg2icon.py`) and those triangle indices are baked
directly into `drawWeatherIcon()`'s `BOLT_PTS`/`BOLT_TRI` arrays in
native-canvas-unit coordinates, positioned at runtime the same way as the
back cloud puff (offset from `(cx, cloudCy)` scaled by `k`). Replaced an
earlier rough two-triangle zigzag that only approximated a bolt shape.

Rain and snow keep their existing procedural circles/lines, just
repositioned against the bitmap cloud's actual bottom edge (previously
tuned against the old three-circle cloud's slightly different bottom) and
given a small size bump alongside the September 2026 icon-size pass below.

The "layered" look amCharts uses for the fully-overcast and thunderstorm
conditions (`cloudy.svg` / `thunder.svg` - a smaller, lighter cloud puff
peeking from behind the main one, pure depth with no separate meaning)
reuses this exact same path a second time, just scaled down and offset -
see `CLOUD_BACK_SCALE`/`CLOUD_BACK_DX`/`CLOUD_BACK_DY` in `main.ino`,
derived from amCharts' own transform on the back copy in those two SVGs.
Partly-cloudy and rain/snow keep a single cloud, matching amCharts'
`cloudy-day/night-*.svg` and `rainy-*.svg` (no back puff in either).
