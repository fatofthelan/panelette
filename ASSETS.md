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

The forecast page's sun/moon/rain/snow/thunderbolt accents are still drawn
procedurally (circles, lines, a filled triangle) - those never had a shape
problem. Only the cloud silhouette itself was replaced: three overlapping
circles never quite reads as a real cloud, no matter how the AA on their
seams is fixed. Tracing amCharts' actual cloud path gets the proportions
right in a way that's impractical to hand-tune from primitives.

This is the first step of a larger forecast-page visual pass (see git log
for the up-to-date state) - not yet done: amCharts' "layered" look (a
smaller lighter cloud puff peeking from behind the main one, used for the
fully-overcast condition) reuses this exact same path at a second scale and
color, so it's a cheap follow-up once the base shape is confirmed to look
right on hardware.
