# HA Panel - Project Context

Custom Home Assistant touch-panel firmware for the ESP32-2432S028R ("CYD" -
Cheap Yellow Display), a 2.8" 240x320 TFT + resistive touchscreen board.
Built from scratch (not based on any third-party sketch) using TFT_eSPI +
XPT2046_Touchscreen. Developed originally in the Arduino IDE, migrated to
PlatformIO / VS Code on 2026-08-30.

This file exists because a lot of the hardest-won findings below came from
multiple rounds of "flash it, photograph the screen, diagnose from that" -
they are not visible from reading the code alone, and re-discovering any of
them wastes real time. Read this before making display/color/layout or
build-config changes.

## Files

- `src/main.ino` - the whole firmware, single file (~4500 lines). It is a
  `.ino`, not a `.cpp`, on purpose - see "auto-prototype gotcha" below.
- `src/config_types.h` - struct definitions + `enum HaConnState` only:
  `TileConfig`, `PageConfig`, `DeviceConfig`, `TileRuntime`, `TzEntry`,
  `WebFontEntry`. These MUST live in a header, not the .ino - see the
  auto-prototype gotcha. Note: the `DeviceConfig cfg` global instance and
  `struct BulbColorEntry` are in `main.ino`, not here.
- `src/NotoSansB18.h` / `NotoSansB30.h` / `PlexMono16.h` / `PlexMono26.h` -
  the anti-aliased `.vlw` fonts as `PROGMEM` byte arrays. See "Fonts".
- `tools/ttf2vlw.py` - the TTF -> `.vlw` converter that generated those
  (freetype-py, no Processing IDE needed). See "Fonts".
- `include/secrets.h` - `WIFI_SSID` / `WIFI_PASSWORD` `#define`s. Gitignored.
  Copy `include/secrets.h.example` to it and fill in real values.
- `platformio.ini` - single env `[env:esp32dev]`. Read its comments before
  changing `build_flags` or `lib_deps`.

## Build environment (PlatformIO)

- **`pio` CLI** is at `~/.platformio/penv/bin/pio` - not on PATH in a
  non-login shell. Build: `~/.platformio/penv/bin/pio run`. Upload:
  `... run -t upload`. Serial: `... device monitor` (115200).
- **`upload_speed = 460800`.** 921600 fails on this board's CH340 USB-serial
  chip. Drop to 230400 if 460800 ever misbehaves.
- **ArduinoJson is pinned to v6** (`^6.21.5`). The sketch uses
  `DynamicJsonDocument(size)`, `StaticJsonDocument<N>`, and `containsKey()`
  everywhere - all removed in ArduinoJson v7. Do not let it float to v7.
- **XPT2046_Touchscreen is pulled from GitHub**
  (`https://github.com/PaulStoffregen/XPT2046_Touchscreen.git`), not the
  PlatformIO registry. The registry build is old and its `begin()` takes no
  arguments; the sketch calls `ts.begin(touchSPI)` with an explicit SPIClass.
- **`links2004/WebSockets @ ^2.4.1`** - the HA WebSocket live-updates client
  (optional feature, off by default). ~27 KB flash.
- **`-D SMOOTH_FONT=1`** in `build_flags` - enables TFT_eSPI `.vlw`
  smooth-font support, which the whole on-device UI now uses.
- Partition table is `min_spiffs.csv` (`board_build.partitions`) - ~1.9 MB
  app + ~190 KB LittleFS; app is at ~68%. Arduino IDE equivalent: Tools ->
  Partition Scheme -> "Minimal SPIFFS". Changing it reformats LittleFS, so
  the on-device config is lost - export/import around any partition change.
- The `TOUCH_CS pin not defined` warning from TFT_eSPI at build time is
  expected and harmless - touch runs through XPT2046_Touchscreen on its own
  SPI bus (VSPI, pins 25/32/33/36), not through TFT_eSPI.

## Screen orientation

Portrait always. `cfg.flipScreen` toggles `tft.setRotation()` between 2
(default) and 0 (180 flip, for USB-port-on-the-other-side mounting) via
`applyScreenRotation()`. **Touch is left on its rotation-2 calibration** -
`readTouchXY()` point-reflects the mapped x/y (`SCREEN_W-1-x`, `SCREEN_H-1-y`)
when flipped, rather than re-calibrating. Toggle is on the Status page and
in the web Device card. If a flip shows a small pixel offset/black band,
that's a CGRAM-offset quirk on some CYD ST7789 units - add `TFT_ROW_OFFSET`
/ `TFT_COL_OFFSET` build flags.

## Display hardware config - this is an ST7789 panel, not ILI9341

Many CYDs ship with an ILI9341; **this one is ST7789** and needs BGR color
order. Wrong driver = garbled or blank screen with **no compile error**.

TFT_eSPI is configured entirely from `platformio.ini` `build_flags`
(`-D USER_SETUP_LOADED=1` plus all the pins/flags), NOT from a `User_Setup.h`
file and NOT from a vendored copy of the library in `lib/`. The build_flags
are a 1:1 port of the `User_Setup.h` that worked under the Arduino IDE. Key
values:

- `ST7789_DRIVER`, `TFT_RGB_ORDER=TFT_BGR`, 240x320
- SPI: `SPI_FREQUENCY=40000000`, `SPI_TOUCH_FREQUENCY=2500000`
- Pins: MISO 12, MOSI 13, SCLK 14, CS 15, DC 2, RST -1, BL 21, backlight
  active HIGH
- Fonts: `LOAD_GLCD`, `LOAD_FONT2/4/6/7/8`, `LOAD_GFXFF` (sketch itself only
  uses 1/2/4 + GFXFF; the rest match the original setup)

`TFT_eSPI` IS listed in `lib_deps` (`bodmer/TFT_eSPI @ ^2.5.43`) - the
`USER_SETUP_LOADED` flag makes the registry copy ignore its own
`User_Setup.h`, so there is no pin-mismatch risk and nothing to vendor.

## Critical: the display renders every color as its bitwise complement

This took several rounds to diagnose, including wrong turns. **Do not
re-litigate without new evidence.**

- Wrong theory (tried first): red/blue channels swapped. A `swapRB565()`
  software helper gave inconsistent results. (Note: the *hardware* BGR order
  IS set via `TFT_RGB_ORDER=TFT_BGR` - that is separate and correct. The
  software RB-swap on top of it was the wrong path.)
- Wrong theory (tried second): `tft.invertDisplay(true)` should fix a
  hardware inversion issue. Confirmed via photos that it has **no effect** on
  this panel/driver combination (see the comment at the `invertDisplay` call
  site in `main.ino`).
- Correct diagnosis: full bitwise inversion, confirmed by decoding
  photographed colors against `NOT(intended)` for THREE independent color
  values - all matched exactly (intended blue photographed as orange,
  intended amber as blue, the timer marquee's intended red as yellow).

**Current fix**: `fixColor565(uint16_t c) { return (uint16_t)(~c); }`, applied
to every color literal at the point it is defined (theme colors in
`applyTheme()`, the `BULB_COLORS` table, `TFT_RED`/`TFT_WHITE` built-ins used
for the marquee / reboot button / switch knob). Any new hardcoded color
needs `fixColor565()` wrapped around it or it renders wrong. Colors built
from already-wrapped colors (e.g. `blendColor565(COL_PANEL, COL_ACCENT, 40)`)
do NOT need re-wrapping.

**Do not add `-D TFT_INVERSION_OFF` (or `_ON`) to fight this** without also
removing `fixColor565()` - the software hack is tuned against the current
build_flags exactly, and changing the hardware inversion would double-invert.

## Arduino / PlatformIO .ino auto-prototype gotcha

Both the Arduino IDE and PlatformIO's `.ino` compatibility layer auto-generate
forward declarations for every function, inserted as one block immediately
after the `#include` lines - regardless of where the function is actually
defined. `main.ino` calls ~90 functions before their definitions and relies
on this. This is why the file stays a `.ino` and is not renamed to `.cpp`
(that would need an explicit ~90-line prototype block added by hand).

It has also caused real compile errors, twice:

1. A function's **signature** uses a custom struct defined later in the .ino -
   the auto-generated prototype then references an unknown type. Fix: struct
   definitions live in `config_types.h`, included first.
2. A function's **body** references a global declared later in the file. Auto-
   prototyping does NOT solve this (it only forward-declares functions, not
   variables) - it is a plain sequential-declaration issue. Fix: when adding
   a block of related globals + functions, declare ALL globals first, THEN
   all functions, never interleaved. This bit the timer/flash-light feature.

If a fresh build throws "X was not declared in this scope" or "redefinition
of Y", check these two patterns first.

Known cosmetic issue: VS Code's C/C++ IntelliSense does not parse `.ino`
files, so it shows spurious squiggles. The build is unaffected. Dismissed
deliberately rather than converting to `.cpp`.

## Fonts (anti-aliased `.vlw`, since the theme redesign)

The on-device UI is drawn entirely in **anti-aliased TFT_eSPI `.vlw`
smooth-fonts** - the old numbered bitmap fonts (1/2/4) and the GFXFF
"Free Fonts" experiment are both gone.

- Two families, one per typeface axis: **Noto Sans Bold** (`uiTypeface` =
  "sans") and **IBM Plex Mono Medium** ("mono"). Each ships as two size
  cuts: a base (~16-18 px, `NotoSansB18` / `PlexMono16`) and a big cut
  (~26-30 px, `NotoSansB30` / `PlexMono26`), embedded as `PROGMEM` byte
  arrays in `src/*.h`.
- **Only one `.vlw` can be loaded at a time.** `applyTypeface()` loads the
  active base cut into both `tft` and `sprTile`; `uiDrawString()` /
  `uiTextWidth()` / `uiDrawFitted()` swap in the big cut (`gFontBig`) for
  the duration of a call when `fontNum >= 4`, then reload the base.
  **Never call `tft.drawString()` / `.setTextFont()` / numbered fonts
  directly** - always go through the `ui*` wrappers.
- `gFontBase` / `gFontBig` point at the active cuts; `gMono` / `gForceUpper`
  (mono uppercases every string) / `gTileRule` (hairline under mono tile
  labels) are set by `applyTypeface()`.
- **`.vlw` generation**: `tools/ttf2vlw.py` (freetype-py, installed into
  `~/.platformio/penv/`). Processing IDE is NOT needed. It writes the v11
  `.vlw` format and computes ascent/descent from the actual rendered ASCII
  glyph extents (not `face.size.ascender`, which came out too tall). To add
  a glyph range or regenerate: edit the `codes` list, run
  `ttf2vlw.py font.ttf out.vlw <px> [wght]`, then hexdump into a
  `const uint8_t NAME_vlw[] PROGMEM` array. Current range is printable ASCII
  0x20-0x7E plus 0xB0 (degree). A malformed `.vlw` can crash on boot - keep
  a known-good copy.
- Layout is tuned against these metrics. `uiDrawFitted()` truncates with
  "..." when text overflows (there is no smaller tile-scale cut to shrink
  to). Real widths are in `ttf2vlw.py`'s output and can be decoded from the
  headers if you need to check a fit.
- GFXFF font headers (`Fonts/GFXFF/*.h`) must NOT be explicitly `#include`d
  even though nothing uses them now - `TFT_eSPI.h` includes them all when
  `LOAD_GFXFF` is set (still in `build_flags`), and re-including caused
  "redefinition" errors (no include guards).

## Theme system (three orthogonal axes + dark/light)

Set in the web UI's Device card, applied by `applyTheme()` /
`applyTypeface()` / `applyCornerStyle()`:

- **`cfg.colorScheme`**: `cool` / `warm` / `phosphor` (default) / `neutral` -
  a row of the `COLOR_SCHEMES[]` table, each with a full dark + light
  `Palette`. `applyTheme(dark)` copies the chosen palette into the `COL_*`
  globals, **each value wrapped in `fixColor565()`** (see the colour
  inversion section).
- **`cfg.uiTypeface`**: `sans` / `mono` (default) - see Fonts.
- **`cfg.cornerStyle`**: `rounded` / `square` (default) - sets `gCardRadius`
  (14 vs 3).
- **Dark/light** is separate (`cfg.darkTheme`), toggled only on the Status
  page. `showTileBorder` is derived: hidden only for rounded+light.
- Defaults (`setDefaultConfig()`): dark + phosphor + mono + square.

## Anti-aliased primitives ("smooth all the things")

Every rounded rect / circle on the device goes through the `ui*` wrappers
in main.ino (`uiFillRR`, `uiStrokeRR`, `uiFillCircle`, `uiRingCircle`),
which call TFT_eSPI's `fillSmoothRoundRect` / `drawSmoothRoundRect` /
`fillSmoothCircle`. Icon rays/hands use `drawWideLine`. **Do not use
`fillRoundRect` / `drawRoundRect` / `fillCircle` / `drawCircle` directly** -
they render jagged.

- The AA edge blends toward a `bg` colour: pass the surface the shape sits
  on (`COL_BG` for straight-to-screen, the local panel/sprite fill
  otherwise). `uiFillCircle` / `fillSmoothCircle` default to `UI_AA_READ`
  (0x00FFFFFF) = sample the actual pixel underneath - correct for sprites,
  slower on `tft` (per-edge-pixel `readPixel` over SPI).
- `drawSmoothRoundRect` / `drawSmoothCircle` do NOT sample bg (there's a
  `// TODO` in TFT_eSPI) - always pass an explicit bg colour to those.
- The brightness overlay's moving track fill is a deliberate exception:
  plain `fillRect`, no AA, because a partial-clear of an AA fill left
  fringe as the level swept.

## Touch handling notes

- Resistive touch (XPT2046) has real contact bounce at press/release
  transitions, and the Z (pressure) reading dips mid-drag. Several guards
  exist because of this:
  - `TOUCH_RELEASE_DEBOUNCE_MS` / the Timers-page `PRESET_GRID_GRACE_MS` -
    stop a bounce (or fast re-tap) from registering as a fresh press on
    whatever UI element just appeared (esp. Timers, where Cancel overlaps
    where the presets reappear).
  - **Tap HA commands are deferred**: `handleTileTap()` queues the command
    (`queueHaOnOff` / `queueHaActivate` / `queueHaBrightness`), and
    `flushPendingHaCommand()` runs it from `loop()` *after* the touch pass.
    The synchronous ~100-400 ms HTTP POST used to land right in the
    debounce window and let the release-bounce double-toggle the tile.
  - **Brightness slider** (`sliderActive` block in `handleContinuousTouch`):
    long-press throws up a large centred overlay (`drawBrightnessOverlay`),
    drag is *relative* to the engage point (anchor Y + anchor %), and it
    only releases after `SLIDER_RELEASE_GRACE_MS` of *sustained* contact
    loss (brief dips are ignored). A long-press released without dragging
    >14 px falls through to a normal tap/toggle - the slider never steals a
    tap. Nothing on the slider path blocks (no network, poll/weather/HA-
    check all bail while `sliderOverlayShown`); brightness goes to HA once,
    on release.
- `HIT_PAD` in `hitTestTile()` intentionally extends tile touch targets a
  few px into the inter-tile gaps, since a near-miss there used to get
  misread as a swipe.
- Tap vs. long-press vs. swipe are disambiguated by `MOVE_TOLERANCE`
  and `LONG_PRESS_MS`.

## Config system

- Settings persist as `/config.json` on LittleFS (not the NVS `Preferences`
  API) - needed because the page/tile lists are variable-length.
- Import/export both exist in the web UI. Export filename is
  `hapanel-<devicename>-<m.d.yyyy>.json` (falls back to `0.0.0` for the date
  if NTP has not synced yet).
- Default device name is `HAPanel` + last 4 hex of the efuse MAC
  (`makeDefaultDeviceName()`), so fresh panels don't collide on `.local`.
  The name is run through `sanitizeHostname()` (alnum + hyphens) on save.
- `weatherLocationAuto` was **removed** - location is imported from HA's
  `/api/config` on save, and the lat/lon/name fields are always editable.
- **Static IP**: `useStaticIp` + `ipAddr`/`subnet`/`gateway`/`dns1`/`dns2`
  (dotted-quad strings). `applyNetworkConfig()` calls `WiFi.config()` in
  `setup()` *before* `WiFi.begin()`. If a static config fails to associate,
  `setup()` retries once on DHCP and sets `staticIpFellBack` (shown as
  `!DHCP` on the Status page IP row and a banner in the web Network card) -
  the stored config is left intact. `validateStaticNet()` checks dotted-quad
  format, contiguous mask, and IP/gateway same-subnet. `/save-network`
  reboots to apply.
- `PageConfig.hidden`: page skipped in the on-device footer + swipe nav.
  Home and Status can never be hidden (`pageCanHide()` - Status is the
  only on-device route to reboot/theme). Nav uses `visiblePageCount()` /
  `nextVisiblePage()` / `prevVisiblePage()` / `visiblePosOf()`; `loop()`
  bounces `currentPageIndex` to Home if it ever lands on a hidden page.
- Some config fields exist with no live UI control (deliberately disabled,
  not forgotten): `bulbColorKey` is forced to "amber" in `resolveBulbColor()`
  regardless of what is stored (the web UI picker was removed after a stale-
  value bug); `webFontChoice` / `uiFontSize` / `uiBoldText` are effectively
  unused now. The tables (`BULB_COLORS`, `WEB_FONTS`) remain in case either
  control comes back.
- Newer fields: `uiTypeface` / `colorScheme` / `cornerStyle` (theme axes),
  `flashOnExpire` (Timers "flash lights" toggle - persisted, defaults on,
  written through on every on-device tap), `marqueeEnabled` (expiry
  screen-border alert, defaults on), `haLiveUpdates` (WebSocket, defaults
  off). `TileConfig.dateEuro` (per-tile D/M/Y vs M/D/Y for date tiles).
  `TileRuntime.unavailable` (not persisted) - set from the HA `state`
  string; the tile shows "N/A" + a struck-through icon.
- **The Device / Home Assistant / Weather cards are three separate `<form>`s**
  that all POST `/save-device`. `handleSaveDevice()` gates each card's
  writes behind a field unique to that form (`deviceName` / `haUrl` /
  `weatherName`) so a partial submit can't clear the others. Same trick in
  `handleSaveFlash()` (gated on `flashRate`).
- Web UI checkboxes render as coloured on/off **toggle switches** (CSS
  `appearance:none` on `.check input[type=checkbox]`); radios are unchanged.
  Backup & Restore is a **segmented Back up / Restore control** (`.seg`,
  JS-toggled panes).
- **Page order**: `cfg.customPageOrder` (bool). When false (default),
  `sortPages()` runs on load / after adding a page and enforces the
  canonical order: Home first, then area pages, then Forecast, Timers,
  Status. When true, `loadConfig()` leaves `cfg.pages[]` exactly as
  stored. `/page/reorder` sets it true; `/page/order-reset` sets it false
  and re-sorts. **Home is always `cfg.pages[0]`** either way - several
  call sites assume it (forecast page name, "area name" field, flash-light
  list all read `cfg.pages[0]`), so Home has no drag handle.
- Footer icons, page dots and swipe nav all just follow `cfg.pages[]`
  array order - no per-view ordering.
- Web UI reordering is **drag-and-drop** (`sortableScript()` - Pointer
  Events, no library; the dragged row goes `position:fixed` and tracks the
  pointer, a `.drop-ph` placeholder marks the target). Tiles:
  `/tile/reorder` takes `order=` as the original 0-based indices in new
  order (validated as a permutation). Pages: `/page/reorder` takes every
  non-Home page id in new order. Both `fetch()` then reload (the
  Edit/Delete buttons carry positional indices). `handleTileMove()` (the
  old up/down `/tile/move`) is left in place but unlinked.

## Home Assistant integration

- **Plain `http://` only, no TLS.** `wss://` / self-signed certs are not
  supported - a valid public cert would work. `haWsUrlSupported()` requires
  the URL start with `http://`.
- **REST is the baseline.** Tiles poll `GET /api/states/<id>` every
  `POLL_INTERVAL_MS` (30 s) while their page is showing (`pollCurrentPageTiles`).
  Only light/switch/sensor tiles poll.
- **Optional WebSocket live updates** (`cfg.haLiveUpdates`, off by default) -
  the `haWs*` module. Holds a `WebSocketsClient` to `/api/websocket`,
  authenticates with the stored token, and `subscribe_entities` (HA
  2022.4+) on the union of every light/switch/sensor entity across all
  pages. HA sends full initial state on subscribe, then compact `a`/`c`
  (added/changed) diffs; `haWsApplyEntitySlice()` pulls `s` (state) and
  `a.brightness` and clears the matching tiles' `cacheKey`. State goes
  through the same code as REST (`applyEntityStateJson` for REST,
  `haWsApplyEntitySlice` for WS). While the socket is `HAWS_READY` the REST
  poll drops to a 150 s backstop and page-open polls are skipped.
  - **One JSON parse per frame.** ArduinoJson parses a non-const
    `uint8_t*` zero-copy (rewrites the buffer in place), so an earlier
    two-pass "peek at type then full parse" corrupted every frame. Parse
    once, size the doc to the frame length.
  - Re-subscribes (full reconnect) when the tracked entity set changes -
    detected by an FNV signature checked every 2 s.
  - Falls back to polling on any failure (`HAWS_FAILED`: https URL / bad
    token; retried every 30 s). `haWsLoop()` is level-triggered, not
    edge-triggered - a settings save calls `haWsStop()` and the loop picks
    it back up. `haWsRawLog` (default false) dumps raw frames to Serial.
  - **"unavailable" latency is HA's, not ours.** After a device loses
    power, how fast HA marks it `unavailable` is that integration's
    availability timeout (ZHA mains devices default to 2 h). The panel
    reflects whatever HA reports - it shows the last known state until HA
    catches up.
- Commands: `haSendCommand()` (`<domain>/turn_on|off`), `haSendBrightness()`
  (`light/turn_on` + `brightness_pct`), `haActivate()` for scene/script/
  button tiles (`scene/turn_on`, `script/turn_on`, `button/press`).
- `haProbeConnection()` = `GET /api/` - distinguishes OK / auth-fail /
  unreachable. Cached in `haConnState`, re-checked every 60 s and on
  demand (web "Test" button -> `/ha-test`, or tapping the Status page HA
  row). `haFetchAndApplyConfig()` = `GET /api/config`, filtered, maps the
  IANA time zone to our key set via `tzKeyFromIana()`.
- **Entity/area discovery goes through `POST /api/template`**, never a bulk
  `GET /api/states` (too big to hold/parse on-device). `haRenderTemplate()`
  sends a Jinja template, gets back short `id|name` lines. Endpoints on the
  panel's own web server (consumed by JS in the tile forms):
  `/ha/entities?domain=`, `/ha/areas`, `/ha/area-entities?area=&domain=`.
  The area one's 3rd field is a group's comma-joined member ids, so the
  browser can pre-uncheck members a group already covers.
- **Every HA-assisted feature falls back to manual entry** on any failure -
  the tile forms keep a plain text field, the settings page keeps manual
  timezone/location, etc. Assume templates/areas() behave per current HA;
  older HA (e.g. `areas()` returning names not ids) just degrades to the
  manual path.

## Decisions made and not revisited without a reason

- Weather comes from Open-Meteo (no API key, no HA dependency), not from a
  Home Assistant weather entity - HA's forecast data now needs a
  `weather.get_forecasts` service call rather than a simple state read, which
  is more complex and version-fragile to support blind.
- scene/script/button tiles are stateless: tap fires the service and shows
  a ~550 ms "Sent" flash (`actionFlash*` globals, cleared in
  `updateCurrentPageDynamic()`); they never poll.
- `showTileBorder` (hairline tile/card border) is hidden only for
  rounded+light; every other combination shows it. Deliberate after mockups.
- Timer light-flash restores each light to its *original* on/off state and
  brightness after flashing (captured via `haFetchEntityState()` before the
  sequence starts), not just "off".
- **The expiry screen-border marquee only flashes once a timer has EXPIRED**
  (nothing while it counts down), until Stop/Restart, and only if
  `cfg.marqueeEnabled`. Earlier it was a solid border for the whole
  countdown.

## What's genuinely unresolved / open

- **No-build-tools install path** (top roadmap item): captive-portal WiFi
  onboarding (removes `secrets.h`), GitHub Releases with a merged
  `firmware.bin`, and an ESP Web Tools page for one-click browser flashing.
  Neither IDE is genuinely "easy" for a non-technical user; this is the
  answer for them. PlatformIO stays the build-from-source path.
- WebSocket live updates: working and shipped, but off by default and still
  proven only on one HA setup. The "unavailable" latency (HA-side, above)
  is the main rough edge; watch free heap over long runs.
- The bulb-color picker and web-font picker are disabled pending a redesign
  without the stale-value problem the old ones had.
- `src/main.ino` could be converted to `.cpp` (add an explicit prototype
  block) to get proper VS Code IntelliSense - not done; build is fine as-is.
- Partition is `min_spiffs.csv` (~1.9 MB app / 190 KB FS) - app is at
  ~68%. Changing partitions reformats LittleFS: export config first.
