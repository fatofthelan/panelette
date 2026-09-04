# Panelette - Project Context

**Panelette** - custom Home Assistant touch-panel firmware for the
ESP32-2432S028R ("CYD" - Cheap Yellow Display), a 2.8" 240x320 TFT +
resistive touchscreen board. (Renamed from "HA Panel" - too generic and
too close to openHASP.)
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
- `include/secrets.h` - OPTIONAL `WIFI_SSID` / `WIFI_PASSWORD` `#define`s.
  Gitignored. `#if __has_include` in main.ino - the firmware builds and
  runs without it (on-device Wi-Fi setup takes over). Copy from
  `secrets.h.example`; the `"your-wifi-ssid"` placeholder counts as not-set.
- `platformio.ini` - one env per CYD panel variant (`esp32dev` = original
  ST7789+invert, `cyd_ili9341`, `cyd_st7789`); shared config in `[env]` +
  `[panel]`. Read its header comment before
  changing `build_flags` or `lib_deps`.
- `docs/` - the GitHub Pages site: `index.html` (ESP Web Tools install
  page), `manifest-<env>.json`, and `firmware/<env>/*.bin` per panel variant.
  `tools/build-installer.sh` regenerates them; `.github/workflows/release.yml`
  runs that on a `v*` tag and cuts a Release.

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

## Display hardware config - the CYD is not one board

"ESP32-2432S028R" ships with an **ST7789 or an ILI9341** panel, **RGB or BGR**
subpixel order, with or without a hardware colour inversion, and occasionally a
CGRAM pixel offset. Wrong driver = garbled or blank (usually white) screen with
**no compile error**. `platformio.ini` has one env per known variant - the pin
map is shared (`[panel] common_flags`); only the driver / colour flags differ:

- **`[env:cyd_ili9341]`** - `default_envs`. ELEGOO (verified) + other ILI9341
  CYDs: `ILI9341_2_DRIVER` (the alt init table CYD folks use for white-screen
  ILI9341s) + `TFT_BGR`, no inversion.
- **`[env:cyd_st7789]`** - "normal" ST7789 CYDs: ST7789 + `TFT_BGR`, no inversion.
- **`[env:esp32dev]`** - the original dev board: ST7789 + `TFT_BGR` +
  `PANEL_INVERT_COLORS` (the bitwise-complement quirk, below). Only this board
  (verified) is known to need it.

Flash a matching env: `pio run -e cyd_ili9341 -t upload`. Symptom guide is in
`platformio.ini`'s header comment. **Touch is also per-panel** - the on-device
3-point wizard runs on first boot and handles it (see "Touch handling notes").

TFT_eSPI is configured entirely from `build_flags` (`-D USER_SETUP_LOADED=1`
plus pins/flags), NOT from a `User_Setup.h` and NOT from a vendored copy in
`lib/`. Shared values: 240x320, `SPI_FREQUENCY=40000000`,
`SPI_TOUCH_FREQUENCY=2500000`, pins MISO 12 / MOSI 13 / SCLK 14 / CS 15 / DC 2 /
RST -1 / BL 21 (active HIGH), fonts `LOAD_GLCD` + `LOAD_FONT2/4/6/7/8` +
`LOAD_GFXFF`. `TFT_eSPI` is in `lib_deps` (`^2.5.43`); `USER_SETUP_LOADED` makes
it ignore its own `User_Setup.h`, so nothing to vendor.

## Critical: the ORIGINAL board renders every color as its bitwise complement

Applies to `[env:esp32dev]` only (it defines `PANEL_INVERT_COLORS`). On the
other envs `fixColor565()` is a pass-through. This took several rounds to
diagnose on the original board, including wrong turns. **Do not re-litigate
without new evidence.**

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

**Current fix**: `fixColor565()` returns `~c` when `PANEL_INVERT_COLORS` is
defined (else `c` unchanged), applied to every color literal at the point it is
defined (theme colors in `applyTheme()`, the `BULB_COLORS` table,
`TFT_RED`/`TFT_WHITE` built-ins used for the marquee / reboot button / switch
knob). Any new hardcoded color needs `fixColor565()` wrapped around it. Colors
built from already-wrapped colors (e.g. `blendColor565(COL_PANEL, COL_ACCENT,
40)`) do NOT need re-wrapping.

**Do not add `-D TFT_INVERSION_OFF` (or `_ON`) to `[env:esp32dev]` to fight
this** without also removing `PANEL_INVERT_COLORS` - the software hack is tuned
against that env's flags exactly, and changing the hardware inversion would
double-invert. A white screen with **negative-image text** on some other board
means `PANEL_INVERT_COLORS` is set when it shouldn't be (or vice versa).

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

- **Calibration is per-panel.** `cfg.touch{Calibrated,SwapXY,XMin,XMax,YMin,
  YMax}` hold the raw->screen map; the compiled `TOUCH_*_MIN/MAX` constants are
  only the defaults for an un-calibrated board. `readTouchXY()` uses the cfg
  values (`map()` copes with a reversed range = inverted axis). The on-device
  3-point wizard (`touchCalLoop()`, module above `setup()`) writes them:
  averages the raw reading at 3 targets (TL/TR/BL), derives swap + per-axis
  span, saves. Triggers: first boot (nothing saved), a finger held ~2.5 s at
  boot, or `POST /calibrate-touch` (web UI Panel -> Touchscreen). 90 s timeout
  keeps the old values. Runs at `setRotation(2)`; the `flipScreen` 180
  point-reflection in `readTouchXY()` is unchanged and layers on top.
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

## Backlight / power-save (`backlightLoop()`, runs every loop, all modes)

Module sits just above `setup()`; its shared globals (`gLastInteractionMs`,
`gBlAsleep`, `gBlSwallowUntilMs`, LDR/level state) are declared up by the
weather sun globals because `readTouchXY()` / `handleContinuousTouch()` (both
earlier in the file) touch them - the auto-prototype gotcha.

- **PWM on GPIO21** (`BACKLIGHT_PIN`, `TFT_BL`, active HIGH). `backlightInit()`
  sets a global 20 kHz `analogWriteFrequency` (above audible - no coil whine)
  before the first `analogWrite`; nothing else in the sketch uses `analogWrite`.
- **`backlightWriteDuty(pct)`** applies a gamma 2.2 curve so low % is genuinely
  dim, with a floor of duty 2 for any pct > 0 (0 = fully off).
- **`cfg.backlightMode`**: `manual` (default, `backlightManualPct`) /
  `schedule` (`backlightHighPct` day / `backlightLowPct` night, window
  `backlightNightFromMin`..`ToMin`, or sunset->sunrise when
  `backlightFollowSun` && `weatherKnown`) / `sensor` (LDR).
- **LDR on GPIO34** (ADC1 - Wi-Fi-safe). Sampled at 5 Hz into `gLdrEma`.
  Sensor mode linearly maps the EMA between `cfg.ldrDarkRaw` and
  `cfg.ldrBrightRaw` -> `backlightLowPct`..`backlightHighPct`. **On the CYD
  the LDR reads HIGH in the dark, LOW in bright** - so `ldrDarkRaw` (~3000) >
  `ldrBrightRaw` (~300); the map derives direction from the two values, so
  there is no "inverted" flag. `GET /ldr` returns `"<raw> <targetPct>
  <currentPct>"` - the web card's calibration ("Set dark / bright point")
  parses the raw; the rest is a diagnostic.
- **Power save** (`cfg.powerSave`, separate toggle): after `powerSaveSec` with
  no touch (`gLastInteractionMs`, bumped in `readTouchXY()`), ramp to
  `powerSaveDimPct` (0 = off). Never sleeps while `timerExpired`. The touch
  that wakes it is **swallowed** - `handleContinuousTouch()` sets
  `gBlSwallowUntilMs = now + 400` and returns early, so the wake tap doesn't
  also toggle a tile or start a swipe.
- `backlightLoop()` re-evaluates the target every 250 ms and ramps `gBlCurrentPct`
  toward it every ~16 ms - fast on wake (0.5 %/ms), gentle otherwise (0.05).
- Web UI: **"Display & Power" card** (own `<form>`, gated on `backlightMode`
  in `handleSaveDevice()`).
- **On-device tiles** (`drawLocalTileSprite()`, local - no HA/entity/poll):
  `powersave` (1x1, tap toggles `cfg.powerSave`) and `backlight` (1x1/1x2,
  tap cycles `manual->schedule->sensor`; wide shows the current level %).
  Both `saveConfig()` + wake the screen on tap.

## Wi-Fi credentials & on-device setup

- **Credentials live in NVS** (`Preferences`, namespace `"wifi"`), NOT in
  `/config.json` - a config reset or LittleFS reformat can't lock the panel
  off the network, and they're never in an exported backup. `wifiCreds*`
  functions. `wifiCredsLoadFromNvs()` opens read-write on purpose (a
  first-ever read on a fresh chip would otherwise log `nvs_open NOT_FOUND`).
- **`wifiResolveCreds()`** at boot picks: compile-time `secrets.h` (real,
  non-placeholder) -> NVS -> none, into `gWifiSsid`/`gWifiPass` +
  `gWifiCredSource`. A `secrets.h` SSID is mirrored into NVS on first boot
  (so a later no-`secrets.h` flash of the same chip still connects).
- **`setup()`**: if creds resolve to NONE, or they won't connect, it calls
  `startProvisioning()` instead of just failing.
- **Provisioning mode** (`gProvisioning`): `WIFI_AP_STA`, open SoftAP named
  `makeDefaultDeviceName()` (`PaneletteXXXX`), `DNSServer` wildcard ->
  192.168.4.1. `loop()` short-circuits to `handleProvisioning()` (serves
  the tiny Wi-Fi-only portal, holds the setup screen, retries stored creds
  every 60 s and reboots on success). `setupWebServer()` registers ONLY the
  portal routes (`/`, `/wifi/save`, `/wifi/scan`, `onNotFound` captive
  redirect) while `gProvisioning`.
- Portal + Improv both do: save to NVS -> `WiFi.begin` -> reboot on
  success. Reboot (not a live hand-off) keeps setup() as the single place
  that brings up NTP / mDNS / HA / the web UI.
- **`/wifi/forget`** (web Network card) clears NVS and reboots; hidden when
  `gWifiCredSource == WCS_COMPILE` (nothing to forget - it's in the binary).

## Improv-Serial (`improvLoop()`, runs every loop, all modes)

- Hand-rolled, ~250 lines, no dependency. Spec: improv-wifi.com/serial.
- Packet: `IMPROV | ver(1) | type(1) | len(1) | data | checksum(1) | \n`.
  Parsing is bounds-checked against `len` (untrusted USB input).
- **Every TX frame ends with `\n`.** ESP Web Tools' serial reader is
  line-oriented (resets its buffer on 0x0A, then looks for the `IMPROV`
  magic); without the terminator it never re-syncs past ESP32 boot-log
  noise and Improv is never detected. `improvSend()` appends it.
- **RPC command IDs must match the spec exactly**: `WIFI_SETTINGS=1`,
  `GET_CURRENT_STATE=2`, `GET_DEVICE_INFO=3`, `GET_WIFI_NETWORKS=4`. (There
  is no serial `IDENTIFY` - 0x02 is `IDENTIFY` only in Improv-BLE.) An
  earlier off-by-one here (`IDENTIFY=2`/`GET_STATE=3`/`GET_INFO=4`) made
  `initialize()`'s device-info request hang, so ESP Web Tools dropped the
  Improv client and showed only Install / Logs.
- Handles `GET_DEVICE_INFO` (`FW_NAME` / `FW_VERSION` / `"ESP32"` / device
  name - this is what lets ESP Web Tools offer *update* vs *install*),
  `GET_CURRENT_STATE` (+ `http://<ip>` when connected), `GET_WIFI_NETWORKS`
  (streamed scan + empty end marker), `WIFI_SETTINGS` (save/connect/reboot,
  or Error).
- `setup()` fires an unprompted `CURRENT_STATE` burst *before* `tft.init()`
  - ESP Web Tools' pre-install probe only waits ~1.5 s from opening the
    port (which reset us). `improvLoop()` keeps a slower announce going for
    ~20 s after boot; any `CURRENT_STATE` frame satisfies the installer.
- **`gImprovProvRtc` (`RTC_NOINIT_ATTR`, survives `ESP.restart()`)**: set on
  a successful `WIFI_SETTINGS` save so the reboot that applies the creds
  stays silent (no `READY` announce) until Wi-Fi reassociates, then sends
  one `PROVISIONED`. Otherwise a still-open ESP Web Tools dialog reads the
  post-reboot `READY` as a failed provision and re-shows the form. Must be
  `RTC_NOINIT_ATTR` - `RTC_DATA_ATTR` is zeroed on every boot.
- `FW_NAME` / `FW_VERSION` `#define`s near the top of main.ino - also the
  serial banner, the Status-page footer, and the web-UI header.

## Config system

- Settings persist as `/config.json` on LittleFS (not the NVS `Preferences`
  API) - needed because the page/tile lists are variable-length. (Wi-Fi
  creds are the exception - see above.)
- Import/export both exist in the web UI. Export filename is
  `panelette-<devicename>-<m.d.yyyy>.json` (falls back to `0.0.0` for the date
  if NTP has not synced yet).
- Default device name is `Panelette` + last 4 hex of the efuse MAC
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
  string; the tile shows "N/A" + a struck-through icon. `backlight*` /
  `ldr*` / `powerSave*` - the auto-dim feature (see the Backlight section).
  `touch*` - the touch-calibration wizard (see Touch handling notes).
- **Web UI is multi-page**, 6 sections: `/` (Overview - status +
  Screenshot/Reboot/Test), `/panel` (device basics + theme + Display&Power),
  `/connection` (HA + Weather + Network + Wi-Fi), `/pages`, `/timers`,
  `/backup`. Shared header via `settingsPageTop(title, navKey)` (heading +
  `settingsNav()` + one-shot `gSaveNotice`) and `settingsFooter()`
  (dirty-guard + close). `settingsNav()` is a 6-cell CSS grid - `repeat(3,1fr)`
  on phones, `repeat(6,1fr)` at >=600px - even both ways. Split keeps the
  biggest single response ~8-14 KB instead of one ~40 KB page.
- **Several cards still POST `/save-device`** (Device, Theme, Home Assistant,
  Weather, Display&Power). `handleSaveDevice()` gates each card's writes
  behind a field unique to its form (`deviceName` / `haUrl` / `weatherName` /
  `backlightMode`; theme/`uiFontSize` fields aren't gated - safe from any
  form). Same trick in `handleSaveFlash()` (gated on `flashRate`).
- Every `/save-*` form carries a hidden `_return`; `redirectAfterSave(fallback)`
  303s back to that section (rejects any non-`/` value). `/page*` handlers
  redirect to `/pages`.
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
  `area-entities` is two passes: domain entities assigned to the area, then
  light/switch *group* entities (they carry an `entity_id` member-list attr)
  with a member in the area but no area of their own. 3rd field is that
  member list; the browser sorts groups first and pre-unchecks covered
  members (`e not in ar` guard stops a group appearing twice).
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

## Browser installer (ESP Web Tools)

- Shipped. `docs/index.html` served by GitHub Pages (`main/docs`).
- **One build per panel variant.** `docs/manifest-<env>.json` +
  `docs/firmware/<env>/` for each of `esp32dev` / `cyd_ili9341` / `cyd_st7789`
  (`docs/manifest.json` is a copy of the `esp32dev` one, for back-compat +
  the page's version fetch). `index.html` has a `<select>` that swaps the
  `<esp-web-install-button>`'s `manifest` attribute (read at click time).
  Default is `cyd_ili9341` (the recommended ELEGOO board); the picker copy
  tells users to re-flash with another option if the screen is wrong.
- **Multi-part manifest** (four parts at their offsets), NOT a merged
  single `.bin` - so an ESP Web Tools *update* writes only those regions
  and leaves NVS (Wi-Fi creds) + the LittleFS config partition alone. A
  first install still full-erases (`new_install_prompt_erase: true`).
- `tools/build-installer.sh` builds all three envs with `secrets.h` moved
  aside and stages `docs/firmware/<env>/` + the manifests.
  `.github/workflows/release.yml` runs it on a `v*` tag, verifies the tag
  matches `FW_VERSION`, commits the refreshed `docs/` to `main`, and cuts a
  Release (shared parts once + `<env>-firmware.bin` per variant).
- `PANEL_VARIANT` (`-D` per env) shows in the serial boot banner and the
  web UI header - so a wrong-panel flash is identifiable.

## What's genuinely unresolved / open

- Browser installer: the ESP Web Tools "Change Wi-Fi" / Improv flow
  (detect -> device info -> scan -> `WIFI_SETTINGS` -> connect -> reboot ->
  dialog shows connected) is now verified end-to-end on hardware against a
  `secrets.h` build. Still unverified: the full *erase* path from a blank
  board (fresh flash -> provisioning SoftAP -> ESP Web Tools Improv screen)
  as one chain, and WPA3-only networks.
- WebSocket live updates: working and shipped, off by default, proven on
  one HA setup. The "unavailable" latency (HA-side) is the rough edge;
  watch free heap over long runs.
- The bulb-color picker and web-font picker are disabled pending a redesign
  without the stale-value problem the old ones had.
- `src/main.ino` could be converted to `.cpp` (add an explicit prototype
  block) to get proper VS Code IntelliSense - not done; build is fine as-is.
- Partition is `min_spiffs.csv` (~1.9 MB app / 190 KB FS) - app is at
  ~69%. Changing partitions reformats LittleFS: export config first.
