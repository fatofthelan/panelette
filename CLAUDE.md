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

- `src/main.ino` - the whole firmware, single file (~3000 lines). It is a
  `.ino`, not a `.cpp`, on purpose - see "auto-prototype gotcha" below.
- `src/config_types.h` - struct definitions only: `TileConfig`, `PageConfig`,
  `DeviceConfig`, `TileRuntime`, `TzEntry`, `WebFontEntry`. These MUST live
  in a header, not the .ino - see the auto-prototype gotcha. Note: the
  `DeviceConfig cfg` global instance and `struct BulbColorEntry` are in
  `main.ino`, not here.
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
- Flash usage is ~90% of the default partition table. If it grows, add
  `board_build.partitions = min_spiffs.csv` (bigger app, smaller FS - fine,
  `config.json` is tiny).
- The `TOUCH_CS pin not defined` warning from TFT_eSPI at build time is
  expected and harmless - touch runs through XPT2046_Touchscreen on its own
  SPI bus (VSPI, pins 25/32/33/36), not through TFT_eSPI.

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

## Font / typeface constraints

- The "Classic" typeface (default) is TFT_eSPI's built-in numbered bitmap
  fonts (1/2/4). Every tile/page layout was pixel-tuned against Classic's
  glyph metrics.
- A "Free Fonts" system (Sans / Serif / Mono / Sans Bold, via TFT_eSPI's
  GFXFF fonts) is selectable in the web UI as an experiment. Different
  (usually wider) metrics, can overflow tiles that fit in Classic.
  `uiDrawFitted()` shrinks-then-truncates text that does not fit, but Free
  Fonts only have two size tiers (9pt/12pt) with no smaller tile-scale
  option, so long labels there truncate with "..." rather than shrink.
- True anti-aliased fonts (TFT_eSPI `.vlw` smooth-font) were considered and
  explicitly NOT pursued - generating a valid `.vlw` needs the Processing
  IDE, and a malformed font file can crash the device on boot with no
  graceful failure. LVGL was discussed as an alternative framework with an
  easier font converter, but adopting it means rebuilding the whole UI - not
  done, just noted as a future option.
- GFXFF font headers (`Fonts/GFXFF/*.h`) must NOT be explicitly `#include`d
  in the .ino - `TFT_eSPI.h` already includes all of them internally when
  `LOAD_GFXFF` is set. Explicit includes caused "redefinition" errors (those
  headers have no include guards).

## Touch handling notes

- Resistive touch (XPT2046) has real contact bounce at press/release
  transitions. `TOUCH_RELEASE_DEBOUNCE_MS` and the Timers-page-specific
  `PRESET_GRID_GRACE_MS` both exist to stop a bounce (or a fast human
  re-tap) from registering as a new press on whatever UI element appears
  after the previous tap's action - especially on the Timers page, where
  Cancel overlaps where the preset buttons reappear.
- `HIT_PAD` in `hitTestTile()` intentionally extends tile touch targets a
  few px into the inter-tile gaps, since a near-miss there used to get
  misread as a swipe.
- Long-press-to-slider (light brightness) vs. tap-to-toggle vs. swipe-to-
  change-page are disambiguated by `MOVE_TOLERANCE` (drift allowed before a
  touch stops counting as a tap) and `LONG_PRESS_MS`.

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
- `PageConfig.hidden`: page skipped in the on-device footer + swipe nav.
  Home and Status can never be hidden (`pageCanHide()` - Status is the
  only on-device route to reboot/theme). Nav uses `visiblePageCount()` /
  `nextVisiblePage()` / `prevVisiblePage()` / `visiblePosOf()`; `loop()`
  bounces `currentPageIndex` to Home if it ever lands on a hidden page.
- Some config fields exist with no live UI control (deliberately disabled,
  not forgotten): `bulbColorKey` is forced to "amber" in `resolveBulbColor()`
  regardless of what is stored (the web UI picker was removed after a stale-
  value bug); `webFontChoice` is unused - `pageHeaderHtml()` hardcodes
  Poppins. The tables (`BULB_COLORS`, `WEB_FONTS`) remain in case either
  control comes back.
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

- **REST only, plain `http://`.** No WebSocket, no TLS. WebSocket live
  updates were considered and deferred (single-core, no-PSRAM MCU; event
  volume + reconnect complexity). `wss://` / self-signed certs are not
  supported - a valid public cert would work.
- Tiles poll `GET /api/states/<id>` every `POLL_INTERVAL_MS` (30 s) while
  their page is showing. Only light/switch/sensor tiles poll.
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
- Dark theme keeps a barely-visible hairline tile border; light theme has no
  border, relying on flat white-vs-gray fill contrast. Deliberate after
  mockups.
- Timer light-flash restores each light to its *original* on/off state and
  brightness after flashing (captured via `haFetchEntityState()` before the
  sequence starts), not just "off".

## What's genuinely unresolved / open

- LVGL / smooth-font migration: discussed, not started.
- The bulb-color picker and web-font picker are disabled pending a redesign
  without the stale-value problem the old ones had.
- `src/main.ino` could be converted to `.cpp` (add an explicit prototype
  block) to get proper VS Code IntelliSense - not done; build is fine as-is.
- WebSocket live state updates: deferred, not ruled out (see the HA
  integration section for why).
- No-build-tools install path (captive-portal WiFi onboarding + prebuilt
  binary + ESP Web Tools flasher page): planned, see INSTRUCTIONS.md.
- Flash is ~92% of the default app partition. If it tops out, switch to
  `board_build.partitions = min_spiffs.csv` - note that reformats LittleFS,
  so export config first.
