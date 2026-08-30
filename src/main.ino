// =========================================================
// HA Panel - Phase 1
// Multi-page HA touch control panel for the CYD (ESP32-2432S028R)
//
// Implemented this phase:
//   - Page/tile data model, stored as /config.json on LittleFS
//   - 2 col x 3 row tile grid per page (1x1 and wide 1x2 tiles)
//   - Swipe left/right between pages, footer page-dot nav
//   - Tile types: light (tap=toggle, hold+drag=brightness slider),
//     switch (tap=toggle), sensor (read-only state)
//   - Status page: network info, dark/light theme toggle, reboot
//   - Web UI: device name, HA URL/token, add/remove pages,
//     add/remove/reorder tiles per page, export config.json
//
// Stubbed for Phase 2:
//   - Forecast page (7-day weather) - placeholder only
//   - Timers page - placeholder only
//   - Config import via web UI
//   - Captive-portal WiFi setup (not needed - manual per-device edit)
// =========================================================

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <TFT_eSPI.h>
// "Free Fonts" (Adafruit-GFX-style outline fonts bundled with TFT_eSPI):
// TFT_eSPI.h -> gfxfont.h already #includes every one of these font files
// itself when LOAD_GFXFF is enabled in User_Setup.h (which this project's
// is). Explicitly re-including them here (as an earlier version of this
// file did) caused duplicate-definition compile errors, since these font
// headers have no include guards. The font objects (FreeSans9pt7b, etc.)
// are already in scope from the TFT_eSPI.h include above - nothing
// further needed here.
#include <time.h>
#include "config_types.h"
#include "secrets.h" // WIFI_SSID / WIFI_PASSWORD - gitignored, copy from secrets.h.example

// =========================================================
// WIFI / HA DEFAULTS
// =========================================================
// WiFi credentials live in include/secrets.h. HA URL/token can also be set
// later from the web UI and are stored in /config.json from then on.
const char* HA_URL_DEFAULT = "http://homeassistant.local:8123";
const char* DEVICE_NAME_DEFAULT = "hapanel";

// =========================================================
// DISPLAY / TOUCH (known-good pins for the CYD ESP32-2432S028R)
// =========================================================
TFT_eSPI tft;

#define TOUCH_CS  33
#define TOUCH_IRQ 36
static const int T_SCK  = 25;
static const int T_MISO = 39;
static const int T_MOSI = 32;

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS);

static const int TOUCH_X_MIN = 562;
static const int TOUCH_X_MAX = 3604;
static const int TOUCH_Y_MIN = 544;
static const int TOUCH_Y_MAX = 3720;

const int BACKLIGHT_PIN = 21;

TFT_eSprite sprTile = TFT_eSprite(&tft);

const int SCREEN_W = 240;
const int SCREEN_H = 320;
const int HEADER_H = 34;
const int FOOTER_H = 44;

// Tile grid geometry - 2 columns x 3 rows, same proportions proven out
// in earlier work on this hardware.
const int COL_X[2]   = {8, 124};
const int GRID_Y[3]  = {42, 120, 198};
const int CELL_W     = 108;
const int CELL_H     = 70;
const int CELL_GAP   = 8; // gap between the two columns

// =========================================================
// THEME
// =========================================================
uint16_t COL_BG, COL_PANEL, COL_PANEL_ALT, COL_STROKE, COL_TEXT, COL_DIM, COL_ACCENT, COL_PANEL_LIT;

// Pushes each channel toward its max value by pct% - used to derive a
// visibly brighter "lit" tile background from the base panel color, so
// it stays in sync automatically if the panel color ever changes.
// Blends toward a target color by pct% - used to derive a visibly "lit"
// tile background by tinting toward the accent color, rather than trying
// to lighten toward white (which does nothing once the panel is already
// white, as it now is in light theme).
static uint16_t blendColor565(uint16_t base, uint16_t target, int pct) {
  pct = constrain(pct, 0, 100);
  int r1 = (base >> 11) & 0x1F, g1 = (base >> 5) & 0x3F, b1 = base & 0x1F;
  int r2 = (target >> 11) & 0x1F, g2 = (target >> 5) & 0x3F, b2 = target & 0x1F;
  int r = r1 + (r2 - r1) * pct / 100;
  int g = g1 + (g2 - g1) * pct / 100;
  int b = b1 + (b2 - b1) * pct / 100;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// This panel displays every color as its bitwise complement relative to
// what's sent (confirmed against two independent, unrelated colors -
// intended blue rendering as orange, intended amber rendering as blue -
// both exactly matching NOT(intended)). tft.invertDisplay() didn't
// correct it, so this compensates in software: wrap every intended color
// with this before it's used, and it'll display as intended.
static uint16_t fixColor565(uint16_t c) {
  return (uint16_t)(~c);
}

// Dark theme keeps a barely-visible hairline border on every card (style
// "C"); light theme drops the border entirely and relies on flat white-
// on-gray fill contrast alone (style "D") - both replacing the earlier
// glowing accent-colored outline on every single element.
bool showTileBorder;

void applyTheme(bool dark) {
  if (dark) {
    COL_BG = fixColor565(0x0000);         // true black
    COL_PANEL = fixColor565(0x39C7);      // #383840 - clearly lighter than bg
    COL_PANEL_ALT = fixColor565(0x4A6A);  // #4C4C52
    COL_STROKE = fixColor565(0x5ACC);     // #5A5A60 - the hairline border color
    COL_TEXT = fixColor565(0xFFFF);
    COL_DIM = fixColor565(0x8C72);        // #8E8E93 (iOS systemGray)
    COL_ACCENT = fixColor565(0x0C3F);     // #0A84FF (iOS dark-mode blue)
  } else {
    COL_BG = fixColor565(0xE73D);         // #E6E6EC - contrast against white cards
    COL_PANEL = fixColor565(0xFFFF);      // white cards
    COL_PANEL_ALT = fixColor565(0xE73C);  // #E5E5EA
    COL_STROKE = fixColor565(0xC639);     // #C7C7CC
    COL_TEXT = fixColor565(0x0000);
    COL_DIM = fixColor565(0x8C72);        // #8E8E93
    COL_ACCENT = fixColor565(0x03DF);     // #007AFF (iOS light-mode blue)
  }
  showTileBorder = dark;
  COL_PANEL_LIT = blendColor565(COL_PANEL, COL_ACCENT, 40); // already-corrected inputs, no extra wrap needed
}

// =========================================================
// TIME ZONE
// =========================================================
const TzEntry TZ_TABLE[] = {
  {"us_pacific",       "US Pacific",        "PST8PDT,M3.2.0/2,M11.1.0/2",       34.0522f,  -118.2437f, "Los Angeles, CA"},
  {"us_mountain",      "US Mountain",       "MST7MDT,M3.2.0/2,M11.1.0/2",       39.7392f,  -104.9903f, "Denver, CO"},
  {"us_arizona",       "US Arizona",        "MST7",                             33.4484f,  -112.0740f, "Phoenix, AZ"},
  {"us_central",       "US Central",        "CST6CDT,M3.2.0/2,M11.1.0/2",       41.8781f,  -87.6298f,  "Chicago, IL"},
  {"us_eastern",       "US Eastern",        "EST5EDT,M3.2.0/2,M11.1.0/2",       40.7128f,  -74.0060f,  "New York, NY"},
  {"alaska",           "Alaska",            "AKST9AKDT,M3.2.0/2,M11.1.0/2",     61.2181f,  -149.9003f, "Anchorage, AK"},
  {"hawaii",            "Hawaii",           "HST10",                            21.3069f,  -157.8583f, "Honolulu, HI"},
  {"utc",               "UTC",              "UTC0",                             51.5074f,  -0.1278f,   "London, UK"},
  {"uk",                "United Kingdom",   "GMT0BST,M3.5.0/1,M10.5.0",         51.5074f,  -0.1278f,   "London, UK"},
  {"europe_central",    "Central Europe",   "CET-1CEST,M3.5.0/2,M10.5.0/3",     52.5200f,  13.4050f,   "Berlin, Germany"},
  {"india",             "India",            "IST-5:30",                        28.6139f,  77.2090f,   "New Delhi, India"},
  {"asia_tokyo",        "Japan",            "JST-9",                           35.6762f,  139.6503f,  "Tokyo, Japan"},
  {"australia_sydney",  "Australia East",   "AEST-10AEDT,M10.1.0,M4.1.0/3",    -33.8688f, 151.2093f,  "Sydney, Australia"},
};
const int TZ_TABLE_COUNT = sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]);

const TzEntry& findTzEntry(const String& key) {
  for (int i = 0; i < TZ_TABLE_COUNT; i++) {
    if (key == TZ_TABLE[i].key) return TZ_TABLE[i];
  }
  return TZ_TABLE[0]; // us_pacific fallback
}

// =========================================================
// WEB UI FONT
// =========================================================
// Loaded client-side from Google Fonts (the browser fetches it over the
// person's own internet connection - the ESP32 only sends a <link> tag,
// it never hosts font files itself).
const WebFontEntry WEB_FONTS[] = {
  {"inter",      "Inter",           "Inter:wght@400;600;700",           "'Inter',sans-serif"},
  {"poppins",    "Poppins",         "Poppins:wght@400;600;700",         "'Poppins',sans-serif"},
  {"montserrat", "Montserrat",      "Montserrat:wght@400;600;700",      "'Montserrat',sans-serif"},
  {"jetbrains",  "JetBrains Mono",  "JetBrains+Mono:wght@400;600",      "'JetBrains Mono',monospace"},
  {"system",     "System Default",  nullptr,                            "-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif"},
};
const int WEB_FONTS_COUNT = sizeof(WEB_FONTS) / sizeof(WEB_FONTS[0]);

const WebFontEntry& findWebFont(const String& key) {
  for (int i = 0; i < WEB_FONTS_COUNT; i++) {
    if (key == WEB_FONTS[i].key) return WEB_FONTS[i];
  }
  return WEB_FONTS[0]; // Inter fallback
}

String sanitizeTimezoneKey(const String& key) {
  for (int i = 0; i < TZ_TABLE_COUNT; i++) {
    if (key == TZ_TABLE[i].key) return key;
  }
  return "us_pacific";
}

const char* timezonePosixByKey(const String& key) {
  for (int i = 0; i < TZ_TABLE_COUNT; i++) {
    if (key == TZ_TABLE[i].key) return TZ_TABLE[i].posix;
  }
  return "PST8PDT,M3.2.0/2,M11.1.0/2"; // us_pacific fallback
}

// =========================================================
// CONFIG DATA MODEL
// =========================================================
// Struct definitions (TileConfig, PageConfig, DeviceConfig, TileRuntime)
// live in config_types.h - see that file for why.

DeviceConfig cfg;
TileRuntime tileRuntime[MAX_PAGES][MAX_TILES];

void applyTimezone() {
  setenv("TZ", timezonePosixByKey(cfg.timezone), 1);
  tzset();
}

// =========================================================
// FONT SIZE
// =========================================================
// TFT_eSPI's built-in fonts only come in fixed jumps (1=8pt, 2=16pt,
// 4=26pt - nothing in between), and tiles are only 108px wide, so font 4
// doesn't reliably fit tile text regardless of preference. Tile text is
// therefore capped at font 2; the Large setting instead grows the header
// and status page, which have more room to spare. Only the font NUMBER
// changes here - button/row positions stay fixed, so this can't
// reintroduce the touch-coordinate or overlap bugs fixed earlier.
int fontTile() { return cfg.uiFontSize == 0 ? 1 : 2; }
int fontHeader() { return cfg.uiFontSize >= 2 ? 4 : 2; }
int fontStatusRow() { return cfg.uiFontSize == 0 ? 1 : 2; }
int fontStatusButton() { return cfg.uiFontSize == 0 ? 1 : (cfg.uiFontSize == 1 ? 2 : 4); }

// =========================================================
// UI TYPEFACE (experimental)
// =========================================================
struct UiTypefaceEntry {
  const char* key;
  const char* label;
};
const UiTypefaceEntry UI_TYPEFACES[] = {
  {"classic",   "Classic (built-in)"},
  {"sans",      "Sans"},
  {"sans_bold", "Sans Bold"},
  {"serif",     "Serif"},
  {"mono",      "Mono"},
};
const int UI_TYPEFACES_COUNT = sizeof(UI_TYPEFACES) / sizeof(UI_TYPEFACES[0]);

const GFXfont* uiFreeFontForRole(int fontNum) {
  bool big = (fontNum >= 4);
  String tf(cfg.uiTypeface);
  if (tf == "sans_bold") return big ? &FreeSansBold12pt7b : &FreeSansBold9pt7b;
  if (tf == "serif")     return big ? &FreeSerif12pt7b     : &FreeSerif9pt7b;
  if (tf == "mono")      return big ? &FreeMono12pt7b      : &FreeMono9pt7b;
  return big ? &FreeSans12pt7b : &FreeSans9pt7b; // "sans" and any unrecognized value
}

// Every on-device text draw goes through this instead of calling
// .drawString() directly, so the typeface/bold experiment applies
// uniformly everywhere without touching each call site's own layout math.
// fontNum keeps its existing meaning (1/2/4 from fontTile()/fontHeader()/
// etc.) - used directly for "classic", or translated to a similar-size
// Free Font otherwise.
//
// EXPERIMENTAL: Free Font glyph metrics don't exactly match the numbered
// bitmap font's, and every tile/page layout in this project was pixel-
// tuned against the numbered font specifically. Switching away from
// "classic" may shift or clip text in spots we haven't hand-tuned for -
// expected while experimenting, not necessarily a new bug.
template<typename T>
void uiDrawString(T& d, const String& text, int x, int y, int fontNum) {
  bool classic = (strlen(cfg.uiTypeface) == 0) || strcmp(cfg.uiTypeface, "classic") == 0;

  if (classic) {
    if (cfg.uiBoldText) d.drawString(text, x + 1, y, fontNum);
    d.drawString(text, x, y, fontNum);
  } else {
    d.setFreeFont(uiFreeFontForRole(fontNum));
    if (cfg.uiBoldText) d.drawString(text, x + 1, y);
    d.drawString(text, x, y);
  }
}

// Mirrors uiDrawString()'s own font-selection logic, so width measurement
// stays correct regardless of which typeface is active - Free Fonts need
// setFreeFont() called before textWidth() measures them correctly, same
// as before actually drawing with them.
template<typename T>
int uiTextWidth(T& d, const String& text, int fontNum) {
  bool classic = (strlen(cfg.uiTypeface) == 0) || strcmp(cfg.uiTypeface, "classic") == 0;
  if (classic) {
    return d.textWidth(text, fontNum);
  } else {
    d.setFreeFont(uiFreeFontForRole(fontNum));
    return d.textWidth(text);
  }
}

// Shortens text (dropping characters from the end, adding "...") until it
// fits within maxWidth. Tries a smaller Classic font size first when
// that's available, since showing the full word smaller beats showing a
// truncated fragment at the original size - Free Fonts don't have a
// smaller tile-scale tier to fall back to, so those go straight to
// truncation. Draws the result directly rather than returning text for
// a separate draw call, since the font actually used can change here.
template<typename T>
void uiDrawFitted(T& d, const String& text, int x, int y, int maxWidth, int preferredFont) {
  bool classic = (strlen(cfg.uiTypeface) == 0) || strcmp(cfg.uiTypeface, "classic") == 0;
  int useFont = preferredFont;

  if (classic && preferredFont > 1 && uiTextWidth(d, text, useFont) > maxWidth) {
    useFont = 1; // Classic has a genuinely smaller size to drop to; Free Fonts don't
  }

  String fitted = text;
  if (uiTextWidth(d, fitted, useFont) > maxWidth) {
    const String ellipsis = "...";
    int ellipsisW = uiTextWidth(d, ellipsis, useFont);
    while (fitted.length() > 1 && uiTextWidth(d, fitted, useFont) + ellipsisW > maxWidth) {
      fitted.remove(fitted.length() - 1);
    }
    fitted += ellipsis;
  }

  uiDrawString(d, fitted, x, y, useFont);
}

// =========================================================
// BULB GLOW COLOR (pickable - see note below)
// =========================================================
// Earlier attempts to fix this by computing the "correct" hex value
// mathematically didn't match what actually showed up on the physical
// screen (traced the R/B-swap compensation precisely and it should have
// produced yellow - it didn't). Rather than keep guessing blind, this
// lets the color be picked empirically from a spread of candidates and
// saved once it's confirmed to actually look right.
struct BulbColorEntry {
  const char* key;
  const char* label;
  uint16_t normalHex; // intended RGB565 - now sent as-is, hardware inversion is corrected globally
};
const BulbColorEntry BULB_COLORS[] = {
  {"amber",        "Amber",        0xFDE0},
  {"gold",         "Gold",         0xFEA0},
  {"yellow",       "Yellow",       0xFF47},
  {"brightyellow", "Bright Yellow", 0xFFE0},
  {"orange",       "Orange",       0xFC60},
  {"warmwhite",    "Warm White",   0xFF9A},
  {"cyan",         "Cyan",         0x073F},
  {"blue",         "Blue",         0x0C3F},
  {"green",        "Green",        0x362B},
};
const int BULB_COLORS_COUNT = sizeof(BULB_COLORS) / sizeof(BULB_COLORS[0]);

uint16_t resolveBulbColor() {
  // Picker UI removed a couple rounds back, but cfg.bulbColorKey wasn't
  // reset at the time - if it was ever set to something other than
  // amber (e.g. testing "Blue" before the picker was disabled), that
  // stale value would keep being used with no way to change it back.
  // Forcing amber here until the picker (or a replacement) comes back.
  return fixColor565(BULB_COLORS[0].normalHex); // "amber"
}

void setDefaultConfig() {
  strlcpy(cfg.deviceName, DEVICE_NAME_DEFAULT, sizeof(cfg.deviceName));
  strlcpy(cfg.haUrl, HA_URL_DEFAULT, sizeof(cfg.haUrl));
  cfg.haToken[0] = '\0';
  cfg.darkTheme = true;
  cfg.use12Hour = false;
  cfg.uiFontSize = 1;
  strlcpy(cfg.uiTypeface, "classic", sizeof(cfg.uiTypeface));
  cfg.uiBoldText = false;
  strlcpy(cfg.bulbColorKey, "amber", sizeof(cfg.bulbColorKey));
  strlcpy(cfg.timezone, "us_pacific", sizeof(cfg.timezone));

  cfg.weatherLocationAuto = true;
  const TzEntry& defaultTz = findTzEntry(cfg.timezone);
  cfg.weatherLat = defaultTz.lat;
  cfg.weatherLon = defaultTz.lon;
  strlcpy(cfg.weatherLocationName, defaultTz.cityLabel, sizeof(cfg.weatherLocationName));
  strlcpy(cfg.webFontChoice, "inter", sizeof(cfg.webFontChoice));

  cfg.flashLightIds[0] = '\0';
  cfg.flashPulseRateMs = 500;
  cfg.flashPulseCount = 5;
  cfg.flashBrightnessPct = 25;

  const int defaultPresetsSec[5] = {60, 300, 600, 900, 1800}; // 1/5/10/15/30 min
  for (int i = 0; i < 5; i++) cfg.timerPresetSec[i] = defaultPresetsSec[i];

  cfg.pageCount = 4;

  strlcpy(cfg.pages[0].id, "home", sizeof(cfg.pages[0].id));
  strlcpy(cfg.pages[0].name, "Home", sizeof(cfg.pages[0].name));
  strlcpy(cfg.pages[0].type, "home", sizeof(cfg.pages[0].type));
  cfg.pages[0].deletable = false;
  cfg.pages[0].tileCount = 0;

  strlcpy(cfg.pages[1].id, "forecast", sizeof(cfg.pages[1].id));
  strlcpy(cfg.pages[1].name, "Forecast", sizeof(cfg.pages[1].name));
  strlcpy(cfg.pages[1].type, "forecast", sizeof(cfg.pages[1].type));
  cfg.pages[1].deletable = false;
  cfg.pages[1].tileCount = 0;

  strlcpy(cfg.pages[2].id, "timers", sizeof(cfg.pages[2].id));
  strlcpy(cfg.pages[2].name, "Timers", sizeof(cfg.pages[2].name));
  strlcpy(cfg.pages[2].type, "timers", sizeof(cfg.pages[2].type));
  cfg.pages[2].deletable = false;
  cfg.pages[2].tileCount = 0;

  strlcpy(cfg.pages[3].id, "status", sizeof(cfg.pages[3].id));
  strlcpy(cfg.pages[3].name, "Status", sizeof(cfg.pages[3].name));
  strlcpy(cfg.pages[3].type, "status", sizeof(cfg.pages[3].type));
  cfg.pages[3].deletable = false;
  cfg.pages[3].tileCount = 0;
}

// =========================================================
// LITTLEFS CONFIG LOAD / SAVE / EXPORT
// =========================================================
bool loadConfig() {
  if (!LittleFS.exists("/config.json")) return false;
  File f = LittleFS.open("/config.json", "r");
  if (!f) return false;

  DynamicJsonDocument doc(12288);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  strlcpy(cfg.deviceName, doc["deviceName"] | DEVICE_NAME_DEFAULT, sizeof(cfg.deviceName));
  strlcpy(cfg.haUrl, doc["haUrl"] | HA_URL_DEFAULT, sizeof(cfg.haUrl));
  strlcpy(cfg.haToken, doc["haToken"] | "", sizeof(cfg.haToken));
  cfg.darkTheme = doc["darkTheme"] | true;
  cfg.use12Hour = doc["use12Hour"] | false;
  cfg.uiFontSize = (uint8_t)constrain((int)(doc["uiFontSize"] | 1), 0, 2);
  strlcpy(cfg.uiTypeface, doc["uiTypeface"] | "classic", sizeof(cfg.uiTypeface));
  cfg.uiBoldText = doc["uiBoldText"] | false;
  strlcpy(cfg.bulbColorKey, doc["bulbColorKey"] | "amber", sizeof(cfg.bulbColorKey));
  strlcpy(cfg.timezone, doc["timezone"] | "us_pacific", sizeof(cfg.timezone));
  strlcpy(cfg.timezone, sanitizeTimezoneKey(cfg.timezone).c_str(), sizeof(cfg.timezone));

  cfg.weatherLocationAuto = doc["weatherLocationAuto"] | true;
  const TzEntry& tzForDefault = findTzEntry(cfg.timezone);
  cfg.weatherLat = doc["weatherLat"] | tzForDefault.lat;
  cfg.weatherLon = doc["weatherLon"] | tzForDefault.lon;
  strlcpy(cfg.weatherLocationName, doc["weatherLocationName"] | tzForDefault.cityLabel, sizeof(cfg.weatherLocationName));
  strlcpy(cfg.webFontChoice, doc["webFontChoice"] | "inter", sizeof(cfg.webFontChoice));

  strlcpy(cfg.flashLightIds, doc["flashLightIds"] | "", sizeof(cfg.flashLightIds));
  cfg.flashPulseRateMs = doc["flashPulseRateMs"] | 500;
  cfg.flashPulseCount = doc["flashPulseCount"] | 5;
  cfg.flashBrightnessPct = constrain((int)(doc["flashBrightnessPct"] | 25), 1, 100);

  {
    const int defaultPresetsSec[5] = {60, 300, 600, 900, 1800};
    JsonArray presetsSecArr = doc["timerPresetSec"].as<JsonArray>();
    JsonArray legacyMinArr = doc["timerPresetMin"].as<JsonArray>(); // pre-seconds-support exports
    for (int i = 0; i < 5; i++) {
      if (i < (int)presetsSecArr.size()) {
        cfg.timerPresetSec[i] = presetsSecArr[i].as<int>();
      } else if (i < (int)legacyMinArr.size()) {
        cfg.timerPresetSec[i] = legacyMinArr[i].as<int>() * 60;
      } else {
        cfg.timerPresetSec[i] = defaultPresetsSec[i];
      }
      if (cfg.timerPresetSec[i] < 1) cfg.timerPresetSec[i] = defaultPresetsSec[i];
    }
  }

  JsonArray pagesArr = doc["pages"].as<JsonArray>();
  cfg.pageCount = 0;
  for (JsonObject p : pagesArr) {
    if (cfg.pageCount >= MAX_PAGES) break;
    PageConfig& pg = cfg.pages[cfg.pageCount];
    strlcpy(pg.id, p["id"] | "", sizeof(pg.id));
    strlcpy(pg.name, p["name"] | "Page", sizeof(pg.name));
    strlcpy(pg.type, p["type"] | "area", sizeof(pg.type));
    pg.deletable = p["deletable"] | true;
    pg.tileCount = 0;

    JsonArray tilesArr = p["tiles"].as<JsonArray>();
    for (JsonObject t : tilesArr) {
      if (pg.tileCount >= MAX_TILES) break;
      TileConfig& tl = pg.tiles[pg.tileCount];
      strlcpy(tl.type, t["type"] | "light", sizeof(tl.type));
      strlcpy(tl.label, t["label"] | "", sizeof(tl.label));
      strlcpy(tl.entityId, t["entity_id"] | "", sizeof(tl.entityId));
      tl.size = (uint8_t)(t["size"] | 1);
      if (tl.size != 2) tl.size = 1;
      pg.tileCount++;
    }
    cfg.pageCount++;
  }

  if (cfg.pageCount == 0) return false;
  return true;
}

void buildConfigJson(JsonDocument& doc) {
  doc["deviceName"] = cfg.deviceName;
  doc["haUrl"] = cfg.haUrl;
  doc["haToken"] = cfg.haToken;
  doc["darkTheme"] = cfg.darkTheme;
  doc["use12Hour"] = cfg.use12Hour;
  doc["uiFontSize"] = cfg.uiFontSize;
  doc["uiTypeface"] = cfg.uiTypeface;
  doc["uiBoldText"] = cfg.uiBoldText;
  doc["bulbColorKey"] = cfg.bulbColorKey;
  doc["timezone"] = cfg.timezone;
  doc["weatherLocationAuto"] = cfg.weatherLocationAuto;
  doc["weatherLat"] = cfg.weatherLat;
  doc["weatherLon"] = cfg.weatherLon;
  doc["weatherLocationName"] = cfg.weatherLocationName;
  doc["webFontChoice"] = cfg.webFontChoice;
  doc["flashLightIds"] = cfg.flashLightIds;
  doc["flashPulseRateMs"] = cfg.flashPulseRateMs;
  doc["flashPulseCount"] = cfg.flashPulseCount;
  doc["flashBrightnessPct"] = cfg.flashBrightnessPct;

  JsonArray presetsOut = doc.createNestedArray("timerPresetSec");
  for (int i = 0; i < 5; i++) presetsOut.add(cfg.timerPresetSec[i]);

  JsonArray pagesArr = doc.createNestedArray("pages");
  for (int i = 0; i < cfg.pageCount; i++) {
    PageConfig& pg = cfg.pages[i];
    JsonObject p = pagesArr.createNestedObject();
    p["id"] = pg.id;
    p["name"] = pg.name;
    p["type"] = pg.type;
    p["deletable"] = pg.deletable;

    JsonArray tilesArr = p.createNestedArray("tiles");
    for (int j = 0; j < pg.tileCount; j++) {
      TileConfig& tl = pg.tiles[j];
      JsonObject t = tilesArr.createNestedObject();
      t["type"] = tl.type;
      t["label"] = tl.label;
      t["entity_id"] = tl.entityId;
      t["size"] = tl.size;
    }
  }
}

bool saveConfig() {
  DynamicJsonDocument doc(12288);
  buildConfigJson(doc);

  File f = LittleFS.open("/config.json", "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

String exportConfigJson() {
  DynamicJsonDocument doc(12288);
  buildConfigJson(doc);
  String out;
  serializeJsonPretty(doc, out);
  return out;
}

// =========================================================
// NAVIGATION / PAGE STATE
// =========================================================
// Moved above WEATHER (rather than its more natural spot further down
// near the touch-handling code) because ensureWeather() needs
// currentPageIndex/pageDirty, and Arduino's auto-generated function
// prototypes are inserted right after the #include block - anything a
// function references has to already be declared by the point that
// function is defined, not just by the point it's used at runtime.
int currentPageIndex = 0;
bool pageDirty = true;
unsigned long lastPollMs = 0;
const unsigned long POLL_INTERVAL_MS = 30UL * 1000UL;

// Light long-press-to-slider state, generalized to (page,tile) instead
// of a flat light array.
int touchTileIndex = -1;
bool touchDown = false;
bool touchMoved = false;
bool sliderActive = false;
unsigned long touchStartMs = 0;
int touchStartX = 0, touchStartY = 0;
int sliderPercent = 0;
unsigned long lastSliderSendMs = 0;

// Horizontal swipe tracking (separate from the tile long-press tracker;
// only armed when a press starts outside any tile, i.e. on empty grid
// space, so it doesn't fight with tile taps/holds).
bool swipeCandidate = false;
int swipeStartX = 0, swipeStartY = 0;

const unsigned long LONG_PRESS_MS = 450;
const int MOVE_TOLERANCE = 20; // resistive touch has some inherent jitter; too tight and taps/holds misfire
const unsigned long SLIDER_SEND_INTERVAL_MS = 250;
const int SWIPE_THRESHOLD_X = 40;

// Resistive touch contact can bounce (touched()/released() flickering a
// few times) right as a finger lifts. Without this, a bounce landing
// right after a tap-triggered action (e.g. Cancel swapping the Timers
// page back to the preset grid) could register as a brand new press on
// whatever now happens to be at that same screen position.
unsigned long lastTouchReleaseMs = 0;
const unsigned long TOUCH_RELEASE_DEBOUNCE_MS = 350;

// Status page "tap again to confirm" reboot state
bool rebootArmed = false;
unsigned long rebootArmedMs = 0;
const unsigned long REBOOT_CONFIRM_WINDOW_MS = 3000;

// =========================================================
// TIMER / RED MARQUEE / LIGHT FLASH
// =========================================================
// All state declared up front, functions after - a function's body
// needs any global it references to already be declared earlier in the
// file (plain C++ sequential-declaration rule, distinct from Arduino's
// auto-prototype mechanism which only helps with calling functions
// defined later, not variables).
bool timerRunning = false;
bool timerExpired = false;
unsigned long timerEndMs = 0;
unsigned long timerDurationSec = 0; // remembered so Restart can reuse it
unsigned long timerRemainingSec = 0;
bool timerFlashOnExpire = false;    // on-device checkbox, per timer session (not persisted)

// Timers page preset buttons now come from cfg.timerPresetSec (editable in
// the web UI) rather than a fixed array here. Presets are stored in
// seconds so they can go below a minute (e.g. a 15-second preset).
String formatPresetLabel(int totalSec) {
  if (totalSec < 60) return String(totalSec) + " sec";
  int mins = totalSec / 60;
  int secs = totalSec % 60;
  if (secs == 0) return String(mins) + " min";
  return String(mins) + "m " + String(secs) + "s";
}

// Tracks what updateTimersCountdownText() (defined later, near the other
// page-update functions) last drew, so it can skip redundant redraws.
String lastTimersCountdownText = "";

// Red border around the whole screen while a timer is running; blinks
// once it expires. Re-asserted on top of whatever page content is
// showing every loop() tick, rather than reserving permanent screen
// space for it.
bool marqueeCurrentlyDrawn = false;
bool marqueeVisible = true;
unsigned long lastMarqueeToggleMs = 0;
const unsigned long MARQUEE_BLINK_MS = 500;

// Non-blocking light-flash sequence, started when a timer expires with
// flashing enabled. Lights are addressed generically via haSendCommand's
// domain-prefix parsing, so this works for both light.* and switch.*.
bool flashSequenceActive = false;
int flashStepsRemaining = 0;
bool flashPhaseOn = false;
unsigned long lastFlashStepMs = 0;

#define MAX_FLASH_LIGHTS 8
String flashLightList[MAX_FLASH_LIGHTS];
bool flashLightOriginalOn[MAX_FLASH_LIGHTS];
int flashLightOriginalBrightness[MAX_FLASH_LIGHTS];
int flashLightCount = 0;

void parseFlashLightIds() {
  flashLightCount = 0;
  String ids(cfg.flashLightIds);
  int start = 0;
  while (start < (int)ids.length() && flashLightCount < MAX_FLASH_LIGHTS) {
    int comma = ids.indexOf(',', start);
    String id = (comma < 0) ? ids.substring(start) : ids.substring(start, comma);
    id.trim();
    if (id.length() > 0) {
      flashLightList[flashLightCount] = id;
      flashLightCount++;
    }
    if (comma < 0) break;
    start = comma + 1;
  }
}

void sendFlashCommand(bool bright) {
  for (int i = 0; i < flashLightCount; i++) {
    String domain = entityDomain(flashLightList[i].c_str());
    if (domain == "light") {
      // "On" phase is always full brightness; the "dim" phase uses the
      // configured floor instead of turning fully off, so flashing at
      // night doesn't plunge the room into darkness.
      haSendBrightness(flashLightList[i].c_str(), bright ? 100 : cfg.flashBrightnessPct);
    } else {
      haSendCommand(flashLightList[i].c_str(), bright);
    }
  }
}

// Sends each light back to whatever it was doing (and, for dimmable
// lights, whatever brightness it was at) before the flash sequence
// started, rather than leaving them all in the last flash phase.
void restoreFlashLights() {
  for (int i = 0; i < flashLightCount; i++) {
    String domain = entityDomain(flashLightList[i].c_str());
    if (!flashLightOriginalOn[i]) {
      haSendCommand(flashLightList[i].c_str(), false);
    } else if (domain == "light") {
      haSendBrightness(flashLightList[i].c_str(), flashLightOriginalBrightness[i]);
    } else {
      haSendCommand(flashLightList[i].c_str(), true);
    }
  }
  flashLightCount = 0;
}

void startFlashSequence() {
  parseFlashLightIds();
  if (flashLightCount == 0) return;

  // Capture each light's current state (and brightness) before flashing
  // so it can be restored afterward - lights that were on go back to on
  // at their original brightness, lights that were off go back to off.
  for (int i = 0; i < flashLightCount; i++) {
    TileRuntime scratch; // not tied to any tile, just a throwaway read
    bool ok = haFetchEntityState(flashLightList[i].c_str(), scratch);
    flashLightOriginalOn[i] = ok ? scratch.on : false;
    flashLightOriginalBrightness[i] = ok ? scratch.brightnessPct : 100;
  }

  flashSequenceActive = true;
  flashStepsRemaining = max(1, cfg.flashPulseCount) * 2;
  flashPhaseOn = true;
  lastFlashStepMs = millis();
  sendFlashCommand(true);
  flashStepsRemaining--;
}

void updateFlashSequence() {
  if (!flashSequenceActive) return;
  if (flashStepsRemaining <= 0) {
    flashSequenceActive = false;
    restoreFlashLights();
    return;
  }

  unsigned long now = millis();
  if (now - lastFlashStepMs < (unsigned long)cfg.flashPulseRateMs) return;

  flashPhaseOn = !flashPhaseOn;
  sendFlashCommand(flashPhaseOn);
  lastFlashStepMs = now;
  flashStepsRemaining--;
}

void startTimer(unsigned long totalSeconds) {
  if (totalSeconds < 1) totalSeconds = 1;
  timerDurationSec = totalSeconds;
  timerRemainingSec = timerDurationSec;
  timerEndMs = millis() + timerDurationSec * 1000UL;
  timerRunning = true;
  timerExpired = false;
  lastTimersCountdownText = "";
  pageDirty = true;
}

void stopTimer() {
  timerRunning = false;
  timerExpired = false;
  if (flashSequenceActive) {
    flashSequenceActive = false;
    restoreFlashLights(); // don't leave lights stuck mid-flash if Stop is tapped early
  }
  pageDirty = true;
}

void restartTimer() {
  if (flashSequenceActive) {
    flashSequenceActive = false;
    restoreFlashLights();
  }
  startTimer(timerDurationSec); // already in seconds - no minimum floor needed anymore
}

void updateMarqueeBorder() {
  bool active = timerRunning || timerExpired;

  if (!active) {
    if (marqueeCurrentlyDrawn) {
      pageDirty = true; // let the normal page redraw clean up the border
      marqueeCurrentlyDrawn = false;
    }
    return;
  }

  bool visible = true;
  if (timerExpired) {
    unsigned long now = millis();
    if (now - lastMarqueeToggleMs >= MARQUEE_BLINK_MS) {
      marqueeVisible = !marqueeVisible;
      lastMarqueeToggleMs = now;
    }
    visible = marqueeVisible;
  }

  uint16_t borderColor = visible ? fixColor565(TFT_RED) : COL_BG;
  tft.drawRect(0, 0, SCREEN_W, SCREEN_H, borderColor);
  tft.drawRect(1, 1, SCREEN_W - 2, SCREEN_H - 2, borderColor);
  tft.drawRect(2, 2, SCREEN_W - 4, SCREEN_H - 4, borderColor);
  marqueeCurrentlyDrawn = true;
}

// Called every loop() tick; only does real work once the timer's actual
// deadline has passed.
void updateTimerState() {
  if (!timerRunning) return;

  long remainMs = (long)(timerEndMs - millis());
  if (remainMs <= 0) {
    timerRunning = false;
    timerExpired = true;
    timerRemainingSec = 0;
    marqueeVisible = true;
    lastMarqueeToggleMs = millis();
    pageDirty = true; // Timers page isn't tile-based, so it needs an explicit refresh trigger
    if (timerFlashOnExpire) startFlashSequence();
  } else {
    timerRemainingSec = (unsigned long)(remainMs + 999) / 1000UL;
  }
}

// =========================================================
// WEATHER (Open-Meteo - no API key required)
// =========================================================
float weatherCurrentTemp = NAN; // already in display units (F or C, see useFahrenheit())
int weatherCurrentCode = -1;    // WMO weather code
bool weatherIsDay = true;       // drives sun vs moon icon variants
bool weatherKnown = false;
time_t lastWeatherFetch = 0;
const uint32_t WEATHER_INTERVAL_SEC = 15UL * 60UL;

struct DailyForecast {
  float tempMax = NAN;
  float tempMin = NAN;
  int code = -1;
  char dayLabel[4] = "---";
};
DailyForecast forecast[5];

// US time zones get Fahrenheit, everything else gets Celsius - matches
// the "aligned to time zone" spirit without adding a separate unit
// setting. Easy to split into its own explicit setting later if wanted.
bool useFahrenheit() {
  String tz(cfg.timezone);
  return tz.startsWith("us_") || tz == "alaska" || tz == "hawaii";
}

bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure(); // no cert store on-device; Open-Meteo has no sensitive data
  HTTPClient http;

  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(cfg.weatherLat, 4) +
               "&longitude=" + String(cfg.weatherLon, 4) +
               "&current=temperature_2m,weather_code,is_day" +
               "&daily=weather_code,temperature_2m_max,temperature_2m_min" +
               "&temperature_unit=" + String(useFahrenheit() ? "fahrenheit" : "celsius") +
               "&forecast_days=5&timezone=auto";

  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(3072);
  if (deserializeJson(doc, body)) return false;

  JsonVariant curTemp = doc["current"]["temperature_2m"];
  JsonVariant curCode = doc["current"]["weather_code"];
  if (curTemp.isNull() || curCode.isNull()) return false;

  weatherCurrentTemp = curTemp.as<float>();
  weatherCurrentCode = curCode.as<int>();
  JsonVariant curIsDay = doc["current"]["is_day"];
  weatherIsDay = curIsDay.isNull() ? true : (curIsDay.as<int>() != 0);

  JsonArray days = doc["daily"]["time"].as<JsonArray>();
  JsonArray codes = doc["daily"]["weather_code"].as<JsonArray>();
  JsonArray tmax = doc["daily"]["temperature_2m_max"].as<JsonArray>();
  JsonArray tmin = doc["daily"]["temperature_2m_min"].as<JsonArray>();

  int count = min((int)days.size(), 5);
  for (int i = 0; i < count; i++) {
    forecast[i].code = codes[i].as<int>();
    forecast[i].tempMax = tmax[i].as<float>();
    forecast[i].tempMin = tmin[i].as<float>();

    String dateStr = days[i].as<String>(); // "2026-08-30"
    int y = dateStr.substring(0, 4).toInt();
    int mo = dateStr.substring(5, 7).toInt();
    int d = dateStr.substring(8, 10).toInt();
    struct tm tmDate = {};
    tmDate.tm_year = y - 1900;
    tmDate.tm_mon = mo - 1;
    tmDate.tm_mday = d;
    tmDate.tm_hour = 12; // noon, avoids DST-boundary edge cases shifting the weekday
    time_t t = mktime(&tmDate);
    struct tm outTm;
    localtime_r(&t, &outTm);
    strftime(forecast[i].dayLabel, sizeof(forecast[i].dayLabel), "%a", &outTm);
  }

  weatherKnown = true;
  lastWeatherFetch = time(nullptr);
  return true;
}

// Called every loop() iteration; only does actual work once the cached
// weather is stale. Weather is global (not per-page like light tiles),
// so this refreshes regardless of which page is currently showing.
void ensureWeather() {
  if (WiFi.status() != WL_CONNECTED) return;

  time_t nowT = time(nullptr);
  if (weatherKnown && (nowT - lastWeatherFetch) < (time_t)WEATHER_INTERVAL_SEC) return;

  if (fetchWeather()) {
    for (int p = 0; p < cfg.pageCount; p++) {
      for (int t = 0; t < cfg.pages[p].tileCount; t++) {
        if (strcmp(cfg.pages[p].tiles[t].type, "weather") == 0) tileRuntime[p][t].cacheKey = "";
      }
    }
    if (strcmp(cfg.pages[currentPageIndex].type, "forecast") == 0) pageDirty = true;
  }
}

String weatherCodeLabel(int code) {
  if (code == 0) return "Clear";
  if (code >= 1 && code <= 3) return "Cloudy";
  if (code == 45 || code == 48) return "Fog";
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "Rain";
  if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) return "Snow";
  if (code >= 95) return "Storm";
  return "--";
}

// Templated so the same code draws into either the real display (tft) or
// a sprite (sprTile) - TFT_eSPI's TFT_eSprite class shares the same
// drawing method names as TFT_eSPI, so instantiating this per call-site
// type is simpler and safer than relying on virtual dispatch.
//
// Uses fixed colors (sun/moon = warm yellow, cloud = light gray with a
// darker outline for shape definition, rain = blue) instead of a single
// external accent color for everything - closer to how most weather
// icons (including Home Assistant's own) are conventionally colored,
// and the outline on the cloud bumps gives it a recognizable puffy
// silhouette instead of reading as one flat blob.
template<typename T>
void drawWeatherIcon(T& d, int cx, int cy, int code, float scale = 1.0f, bool isDay = true, uint16_t cutColor = 0x0000) {
  auto S = [scale](int v) { return (int)roundf(v * scale); };
  uint16_t sunColor = fixColor565(0xFDE0);
  uint16_t cloudFill = fixColor565(0xCE7A);
  uint16_t cloudStroke = fixColor565(0x8C72);
  uint16_t rainColor = fixColor565(0x44FF);

  if (code == 0) {
    if (isDay) {
      // Clear / sunny
      d.fillCircle(cx, cy, S(9), sunColor);
      d.drawLine(cx, cy - S(15), cx, cy - S(11), sunColor);
      d.drawLine(cx, cy + S(11), cx, cy + S(15), sunColor);
      d.drawLine(cx - S(15), cy, cx - S(11), cy, sunColor);
      d.drawLine(cx + S(11), cy, cx + S(15), cy, sunColor);
      d.drawLine(cx - S(11), cy - S(11), cx - S(8), cy - S(8), sunColor);
      d.drawLine(cx + S(11), cy - S(11), cx + S(8), cy - S(8), sunColor);
      d.drawLine(cx - S(11), cy + S(11), cx - S(8), cy + S(8), sunColor);
      d.drawLine(cx + S(11), cy + S(11), cx + S(8), cy + S(8), sunColor);
    } else {
      // Clear / night - crescent moon: a filled disc with a second disc,
      // in the background color, punched out of one side.
      d.fillCircle(cx, cy, S(9), sunColor);
      d.fillCircle(cx + S(5), cy - S(3), S(8), cutColor);
    }
    return;
  }

  if (code == 45 || code == 48) {
    // Fog
    for (int i = 0; i < 4; i++) {
      d.drawFastHLine(cx - S(12), cy - S(6) + i * S(5), S(24), cloudFill);
    }
    return;
  }

  int cloudCy = cy - S(2);
  if (code >= 1 && code <= 3) {
    d.fillCircle(cx - S(7), cloudCy - S(7), S(6), sunColor); // sun/moon peeking out for partly-cloudy
    if (!isDay) {
      d.fillCircle(cx - S(4), cloudCy - S(9), S(5), cutColor); // crescent cutout
    }
  }

  // Cloud shape - base for partly-cloudy, overcast, rain, snow, storm.
  // Filled first, then each bump's own outline drawn on top - the
  // outline is what actually reads as "cloud" rather than a blob, since
  // three same-color filled circles alone merge into one shapeless mass.
  d.fillCircle(cx - S(6), cloudCy, S(7), cloudFill);
  d.fillCircle(cx + S(5), cloudCy, S(8), cloudFill);
  d.fillCircle(cx, cloudCy - S(4), S(7), cloudFill);
  d.fillRoundRect(cx - S(10), cloudCy, S(22), S(8), S(4), cloudFill);
  d.drawCircle(cx - S(6), cloudCy, S(7), cloudStroke);
  d.drawCircle(cx + S(5), cloudCy, S(8), cloudStroke);
  d.drawCircle(cx, cloudCy - S(4), S(7), cloudStroke);
  d.drawRoundRect(cx - S(10), cloudCy, S(22), S(8), S(4), cloudStroke);

  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    // Rain
    for (int i = -1; i <= 1; i++) {
      d.drawLine(cx + i * S(7), cloudCy + S(9), cx + i * S(7) - S(2), cloudCy + S(15), rainColor);
    }
  } else if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) {
    // Snow
    for (int i = -1; i <= 1; i++) {
      d.fillCircle(cx + i * S(7), cloudCy + S(12), max(1, S(1)), cloudFill);
    }
  } else if (code >= 95) {
    // Thunderstorm - simple bolt
    d.fillTriangle(cx - S(2), cloudCy + S(8), cx + S(4), cloudCy + S(8), cx - S(1), cloudCy + S(16), sunColor);
    d.fillTriangle(cx - S(1), cloudCy + S(16), cx + S(5), cloudCy + S(12), cx + S(2), cloudCy + S(20), sunColor);
  }
}

// Simple clock face for the Timer tile type.
template<typename T>
void drawClockIcon(T& d, int cx, int cy, uint16_t iconColor, float scale = 1.0f) {
  auto S = [scale](int v) { return (int)roundf(v * scale); };
  int r = S(12);

  d.drawCircle(cx, cy, r, iconColor);

  // Small tick marks at 12/3/6/9, like a clean SF Symbols-style clock
  // face, instead of a bare outline.
  int tickLen = S(2);
  d.drawFastVLine(cx, cy - r, tickLen, iconColor);
  d.drawFastVLine(cx, cy + r - tickLen, tickLen, iconColor);
  d.drawFastHLine(cx - r, cy, tickLen, iconColor);
  d.drawFastHLine(cx + r - tickLen, cy, tickLen, iconColor);

  // Hands with a small filled hub, drawn thicker via two adjacent lines
  // rather than TFT_eSPI's thin single-pixel drawLine.
  int minX = cx, minY = cy - S(8);
  int hrX = cx + S(6), hrY = cy + S(2);
  d.drawLine(cx, cy, minX, minY, iconColor);
  d.drawLine(cx + 1, cy, minX + 1, minY, iconColor);
  d.drawLine(cx, cy, hrX, hrY, iconColor);
  d.drawLine(cx, cy + 1, hrX, hrY + 1, iconColor);
  d.fillCircle(cx, cy, S(2), iconColor);
}

// =========================================================
// TOUCH READ
// =========================================================
bool readTouchXY(int& x, int& y) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();

  long rawX = constrain((long)p.x, (long)TOUCH_X_MIN, (long)TOUCH_X_MAX);
  long rawY = constrain((long)p.y, (long)TOUCH_Y_MIN, (long)TOUCH_Y_MAX);

  x = (int)map(rawX, TOUCH_X_MIN, TOUCH_X_MAX, 0, SCREEN_W);
  y = (int)map(rawY, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, SCREEN_H);
  x = constrain(x, 0, SCREEN_W - 1);
  y = constrain(y, 0, SCREEN_H - 1);
  return true;
}

// =========================================================
// HOME ASSISTANT API (domain-aware: works for light.* and switch.*)
// =========================================================
static bool haConfigured() {
  return strlen(cfg.haUrl) > 0 && strlen(cfg.haToken) > 0;
}

String entityDomain(const char* entityId) {
  String e(entityId);
  int dot = e.indexOf('.');
  if (dot < 0) return "";
  return e.substring(0, dot);
}

bool haSendCommand(const char* entityId, bool turnOn) {
  if (!haConfigured() || WiFi.status() != WL_CONNECTED) return false;
  String domain = entityDomain(entityId);
  if (domain.length() == 0) return false;

  WiFiClient client;
  HTTPClient http;
  String url = String(cfg.haUrl) + "/api/services/" + domain + "/" + (turnOn ? "turn_on" : "turn_off");
  if (!http.begin(client, url)) return false;

  http.addHeader("Authorization", "Bearer " + String(cfg.haToken));
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<128> doc;
  doc["entity_id"] = entityId;
  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  http.end();
  return code == 200 || code == 201;
}

bool haSendBrightness(const char* entityId, int pct) {
  if (!haConfigured() || WiFi.status() != WL_CONNECTED) return false;
  pct = constrain(pct, 0, 100);
  if (pct <= 0) return haSendCommand(entityId, false);

  WiFiClient client;
  HTTPClient http;
  String url = String(cfg.haUrl) + "/api/services/light/turn_on";
  if (!http.begin(client, url)) return false;

  http.addHeader("Authorization", "Bearer " + String(cfg.haToken));
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<160> doc;
  doc["entity_id"] = entityId;
  doc["brightness_pct"] = pct;
  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  http.end();
  return code == 200 || code == 201;
}

// Fetches state (+ brightness, when present) for any entity into rt.
bool haFetchEntityState(const char* entityId, TileRuntime& rt) {
  if (!haConfigured() || WiFi.status() != WL_CONNECTED) return false;
  if (strlen(entityId) == 0) return false;

  WiFiClient client;
  HTTPClient http;
  String url = String(cfg.haUrl) + "/api/states/" + entityId;
  if (!http.begin(client, url)) return false;

  http.addHeader("Authorization", "Bearer " + String(cfg.haToken));

  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, body)) return false;

  const char* state = doc["state"];
  if (!state) return false;

  rt.lastRawState = String(state);
  rt.on = (rt.lastRawState == "on");
  rt.known = true;

  JsonVariant brightnessVar = doc["attributes"]["brightness"];
  if (!brightnessVar.isNull()) {
    int b255 = brightnessVar.as<int>();
    rt.brightnessPct = constrain((int)((b255 * 100L + 127L) / 255L), 1, 100);
  } else if (rt.on) {
    rt.brightnessPct = 100;
  }

  return true;
}

// =========================================================
// TILE LAYOUT: map ordered tiles (1 or 2 cells wide) onto the 6 grid
// slots, aligning wide tiles to the start of a row.
// =========================================================
void layoutPageTiles(PageConfig& pg, int outSlot[MAX_TILES]) {
  int cursor = 0;
  for (int i = 0; i < pg.tileCount; i++) {
    int size = pg.tiles[i].size;
    if (size == 2) {
      if (cursor % 2 == 1) cursor++;
      if (cursor >= 6) { outSlot[i] = -1; continue; }
      outSlot[i] = cursor;
      cursor += 2;
    } else {
      if (cursor >= 6) { outSlot[i] = -1; continue; }
      outSlot[i] = cursor;
      cursor += 1;
    }
  }
}

void getSlotRect(int slot, bool wide, int& x, int& y, int& w, int& h) {
  int row = slot / 2, col = slot % 2;
  x = COL_X[col];
  y = GRID_Y[row];
  w = wide ? (CELL_W * 2 + CELL_GAP) : CELL_W;
  h = CELL_H;
}

// =========================================================
// DRAW HELPERS
// =========================================================
static uint16_t scaleColor565(uint16_t color, int pct) {
  pct = constrain(pct, 15, 100);
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5) & 0x3F;
  uint8_t b = color & 0x1F;
  r = (uint8_t)((r * pct) / 100);
  g = (uint8_t)((g * pct) / 100);
  b = (uint8_t)((b * pct) / 100);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

void drawBulbIcon(TFT_eSprite& spr, int cx, int cy, bool on, int brightnessPct, uint16_t onColor) {
  uint16_t glassColor = on ? scaleColor565(onColor, brightnessPct) : COL_PANEL_ALT;
  uint16_t strokeColor = on ? onColor : COL_STROKE;
  uint16_t baseColor = on ? COL_DIM : COL_STROKE;
  int gcy = cy - 3; // glass center, shifted up slightly to leave room for the base below

  // Glow rays, drawn behind the glass so the glass's outline reads clean
  // on top - a fuller spread (7 directions, skipping straight-down since
  // light doesn't glow through its own base) rather than 4 short stubs.
  if (on) {
    spr.drawLine(cx, gcy - 16, cx, gcy - 13, onColor);           // N
    spr.drawLine(cx - 16, gcy, cx - 13, gcy, onColor);           // W
    spr.drawLine(cx + 13, gcy, cx + 16, gcy, onColor);           // E
    spr.drawLine(cx - 13, gcy - 13, cx - 10, gcy - 10, onColor); // NW
    spr.drawLine(cx + 13, gcy - 13, cx + 10, gcy - 10, onColor); // NE
    spr.drawLine(cx - 13, gcy + 9, cx - 10, gcy + 7, onColor);   // SW, shallow
    spr.drawLine(cx + 13, gcy + 9, cx + 10, gcy + 7, onColor);   // SE, shallow
  }

  // Glass (bulb head) - drawn with a two-circle outline for visible
  // weight rather than TFT_eSPI's thin single-pixel drawCircle.
  spr.fillCircle(cx, gcy, 11, glassColor);
  spr.drawCircle(cx, gcy, 11, strokeColor);
  spr.drawCircle(cx, gcy, 10, strokeColor);

  // A soft filament hint when lit - a small V inside the glass, like the
  // classic light-bulb silhouette rather than a blank circle.
  if (on) {
    spr.drawLine(cx - 4, gcy - 3, cx, gcy + 3, strokeColor);
    spr.drawLine(cx, gcy + 3, cx + 4, gcy - 3, strokeColor);
  }

  // Tapered neck into a short screw-thread base
  spr.fillRoundRect(cx - 5, gcy + 8, 10, 4, 2, baseColor);
  spr.drawFastHLine(cx - 5, gcy + 12, 10, baseColor);
  spr.drawFastHLine(cx - 4, gcy + 14, 8, baseColor);
}

void drawSwitchIcon(TFT_eSprite& spr, int cx, int cy, bool on, uint16_t onColor) {
  uint16_t trackColor = on ? onColor : COL_PANEL_ALT;
  spr.fillRoundRect(cx - 16, cy - 9, 32, 18, 9, trackColor);
  spr.drawRoundRect(cx - 16, cy - 9, 32, 18, 9, on ? onColor : COL_STROKE);
  spr.drawRoundRect(cx - 15, cy - 8, 30, 16, 8, on ? onColor : COL_STROKE); // thickened outline
  int knobX = on ? (cx + 7) : (cx - 7);
  // Real iOS switches always use a white knob regardless of on/off or
  // theme - using the theme background color here (as before) would
  // make the knob a dark cutout in dark mode, which reads backwards.
  spr.fillCircle(knobX, cy, 7, fixColor565(TFT_WHITE));
  spr.drawCircle(knobX, cy, 7, COL_STROKE);
}

void makeSpriteCard(TFT_eSprite& spr, int w, int h, int fillColorOverride = -1) {
  uint16_t fillColor = (fillColorOverride >= 0) ? (uint16_t)fillColorOverride : COL_PANEL;
  spr.deleteSprite();
  spr.setColorDepth(16);
  spr.createSprite(w, h);
  // Sprite memory isn't cleared by createSprite() - fillRoundRect only
  // paints the rounded shape itself, leaving the four small corner
  // regions outside the curve showing whatever was left over from the
  // sprite's previous use. Filling the whole canvas with the page
  // background first makes those corners blend in instead of showing
  // stale pixel garbage.
  spr.fillSprite(COL_BG);
  spr.fillRoundRect(0, 0, w, h, 14, fillColor);
  if (showTileBorder) spr.drawRoundRect(0, 0, w, h, 14, COL_STROKE);
}

void pushSpriteAndDelete(TFT_eSprite& spr, int x, int y) {
  spr.pushSprite(x, y);
  spr.deleteSprite();
}

// Icon dimmed behind, temperature overlaid on top of it (opaque text
// background creates a "badge" effect since TFT_eSPI sprites don't do
// true alpha blending), condition word at the bottom like other tiles.
void drawTimerTileSprite(int tileIdx, int x, int y, int w, int h, bool wide, bool force) {
  TileRuntime& rt = tileRuntime[currentPageIndex][tileIdx];

  String timeText = "--:--";
  if (timerRunning || timerExpired) {
    unsigned long mins = timerRemainingSec / 60UL;
    unsigned long secs = timerRemainingSec % 60UL;
    char buf[8];
    snprintf(buf, sizeof(buf), "%lu:%02lu", mins, secs);
    timeText = String(buf);
  }

  String combined = timeText + "|" + String(timerRunning ? 1 : 0) + "|" + String(COL_PANEL) + "|" + String(COL_ACCENT);
  if (!force && combined == rt.cacheKey) return;
  rt.cacheKey = combined;

  makeSpriteCard(sprTile, w, h);

  int tempFont = fontHeader();
  sprTile.setTextDatum(TC_DATUM);
  sprTile.setTextColor(COL_TEXT, COL_PANEL);
  uiDrawString(sprTile, timeText, w / 2, 4, tempFont);

  int iconCx = wide ? w / 4 : w / 2;
  // h-22 keeps the icon's bottom edge clear of the tile's bottom border
  // at this scale - the previous h/2+14 put it right at the edge.
  int iconCy = h - 22;
  drawClockIcon(sprTile, iconCx, iconCy, timerRunning ? COL_ACCENT : COL_DIM, 1.3f);

  pushSpriteAndDelete(sprTile, x, y);
}

void drawWeatherTileSprite(int tileIdx, int x, int y, int w, int h, bool wide, bool force) {
  TileRuntime& rt = tileRuntime[currentPageIndex][tileIdx];

  String tempText = weatherKnown ? String((int)roundf(weatherCurrentTemp)) : "...";

  String combined = tempText + "|" + String(weatherCurrentCode) + "|" + String(weatherIsDay ? 1 : 0) + "|" + String(COL_PANEL) + "|" + String(COL_ACCENT);
  if (!force && combined == rt.cacheKey) return;
  rt.cacheKey = combined;

  makeSpriteCard(sprTile, w, h);

  // No label - the temperature takes that spot instead, and the icon
  // (now full-color rather than dimmed, since nothing overlaps it
  // anymore) gets most of the remaining space to be bigger.
  int tempFont = fontHeader(); // 2 at Small/Medium, 4 at Large
  sprTile.setTextDatum(TC_DATUM);
  sprTile.setTextColor(COL_TEXT, COL_PANEL);
  int numW = sprTile.textWidth(tempText, tempFont);
  uiDrawString(sprTile, tempText, w / 2, 4, tempFont);
  sprTile.drawCircle(w / 2 + numW / 2 + 5, tempFont >= 4 ? 9 : 7, 2, COL_TEXT);

  int iconCx = wide ? w / 4 : w / 2;
  // h-22 keeps the icon's bottom edge clear of the tile's bottom border
  // at this scale - the previous h/2+14 put it right at the edge.
  int iconCy = h - 22;

  if (weatherKnown) {
    drawWeatherIcon(sprTile, iconCx, iconCy, weatherCurrentCode, 1.3f, weatherIsDay, COL_PANEL);
  }

  pushSpriteAndDelete(sprTile, x, y);
}

void drawTileSprite(int tileIdx, int slot, bool wide, bool force) {
  if (slot < 0) return;
  PageConfig& pg = cfg.pages[currentPageIndex];
  TileConfig& tl = pg.tiles[tileIdx];
  TileRuntime& rt = tileRuntime[currentPageIndex][tileIdx];

  int x, y, w, h;
  getSlotRect(slot, wide, x, y, w, h);

  // Slider view takes over drawing for the actively-held light tile.
  if (sliderActive && touchTileIndex == tileIdx) return;

  if (strcmp(tl.type, "weather") == 0) {
    drawWeatherTileSprite(tileIdx, x, y, w, h, wide, force);
    return;
  }
  if (strcmp(tl.type, "timer") == 0) {
    drawTimerTileSprite(tileIdx, x, y, w, h, wide, force);
    return;
  }

  bool isLight = (strcmp(tl.type, "light") == 0);
  bool isSwitch = (strcmp(tl.type, "switch") == 0);
  bool isSensor = (strcmp(tl.type, "sensor") == 0);

  String stateText;
  if (strlen(tl.entityId) == 0) {
    stateText = "Unset";
  } else if (!haConfigured()) {
    stateText = "No HA";
  } else if (!rt.known) {
    stateText = "...";
  } else if (isSensor) {
    stateText = rt.lastRawState;
  } else if (isLight) {
    stateText = rt.on ? (String(rt.brightnessPct) + "%") : "Off";
  } else {
    stateText = rt.on ? "On" : "Off";
  }

  String combined = String(tl.label) + "|" + stateText + "|" + String(rt.on ? 1 : 0) + "|" +
                     String(COL_PANEL) + "|" + String(COL_ACCENT);
  if (!force && combined == rt.cacheKey) return;
  rt.cacheKey = combined;

  // Lit-up background for a light that's on, so state is visible even
  // without reading the text - dark for off, brighter for on.
  bool useLitBg = isLight && rt.on;
  uint16_t bgColor = useLitBg ? COL_PANEL_LIT : COL_PANEL;
  makeSpriteCard(sprTile, w, h, useLitBg ? (int)COL_PANEL_LIT : -1);

  sprTile.setTextDatum(TC_DATUM);
  sprTile.setTextColor(COL_DIM, bgColor);
  uiDrawFitted(sprTile, tl.label, w / 2, 4, w - 12, fontTile());

  const uint16_t warmColor = resolveBulbColor();
  int iconCx = wide ? w / 4 : w / 2;
  const int iconCy = 38; // nudged down 2px - the bigger bulb glass needs a touch more clearance from the label above

  if (isLight) {
    drawBulbIcon(sprTile, iconCx, iconCy, rt.on, rt.brightnessPct, warmColor);
  } else if (isSwitch) {
    drawSwitchIcon(sprTile, iconCx, iconCy, rt.on, COL_ACCENT);
  } else {
    sprTile.drawCircle(iconCx, iconCy, 9, COL_DIM);
  }

  sprTile.setTextDatum(MC_DATUM);
  sprTile.setTextColor(rt.on ? COL_TEXT : COL_DIM, bgColor);
  uiDrawString(sprTile, stateText, w / 2, h - 12, fontTile());
  sprTile.setTextDatum(TL_DATUM);

  pushSpriteAndDelete(sprTile, x, y);
}

void drawSliderSprite(int tileIdx, int slot, bool wide, int percent, bool force) {
  if (slot < 0) return;
  PageConfig& pg = cfg.pages[currentPageIndex];
  TileConfig& tl = pg.tiles[tileIdx];
  TileRuntime& rt = tileRuntime[currentPageIndex][tileIdx];

  int x, y, w, h;
  getSlotRect(slot, wide, x, y, w, h);

  String combined = "SLIDER|" + String(percent);
  if (!force && combined == rt.cacheKey) return;
  rt.cacheKey = combined;

  makeSpriteCard(sprTile, w, h);

  sprTile.setTextDatum(TL_DATUM);
  sprTile.setTextColor(COL_DIM, COL_PANEL);
  uiDrawFitted(sprTile, tl.label, 6, 4, w - 50, fontTile());

  sprTile.setTextDatum(TR_DATUM);
  sprTile.setTextColor(COL_ACCENT, COL_PANEL);
  uiDrawString(sprTile, String(percent) + "%", w - 6, 4, fontTile());

  const int trackW = 22;
  const int trackX = (w - trackW) / 2;
  const int trackY = 18;
  const int trackH = h - trackY - 8;

  sprTile.drawRoundRect(trackX, trackY, trackW, trackH, 6, COL_STROKE);
  int innerH = trackH - 4;
  int fillH = (innerH * percent) / 100;
  if (fillH > 0) {
    sprTile.fillRoundRect(trackX + 2, trackY + trackH - 2 - fillH, trackW - 4, fillH, 4, COL_ACCENT);
  }
  int handleY = trackY + trackH - 2 - fillH;
  sprTile.drawFastHLine(trackX - 3, handleY, trackW + 6, COL_TEXT);

  sprTile.setTextDatum(TL_DATUM);
  pushSpriteAndDelete(sprTile, x, y);
}

// =========================================================
// HEADER / FOOTER
// =========================================================
String lastHeaderTimeText = "";

void drawHeader(bool force) {
  PageConfig& pg = cfg.pages[currentPageIndex];

  char timeBuf[12] = "--:--";
  time_t now = time(nullptr);
  if (now > 1700000000) { // sane epoch => NTP has synced
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    if (cfg.use12Hour) {
      int hour12 = tmNow.tm_hour % 12;
      if (hour12 == 0) hour12 = 12;
      snprintf(timeBuf, sizeof(timeBuf), "%d:%02d %s", hour12, tmNow.tm_min, tmNow.tm_hour >= 12 ? "PM" : "AM");
    } else {
      strftime(timeBuf, sizeof(timeBuf), "%H:%M", &tmNow);
    }
  }
  String timeText(timeBuf);
  int hFont = fontHeader();
  int titleY = (hFont == 4) ? 4 : 9;

  if (force) {
    tft.fillRect(0, 0, SCREEN_W, HEADER_H, COL_BG);
    tft.drawFastHLine(0, HEADER_H - 1, SCREEN_W, COL_STROKE);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_TEXT, COL_BG);
    uiDrawString(tft, pg.name, 8, titleY, hFont);
    lastHeaderTimeText = "";
  }

  // 12-hour text ("11:32 PM") needs a bit more room than 24-hour ("23:32"),
  // and font 4 needs more room than font 2 for the same text.
  int clockAreaW;
  if (hFont == 4) {
    clockAreaW = cfg.use12Hour ? 132 : 84;
  } else {
    clockAreaW = cfg.use12Hour ? 100 : 62;
  }

  if (force || timeText != lastHeaderTimeText) {
    tft.fillRect(SCREEN_W - clockAreaW, 0, clockAreaW, HEADER_H - 1, COL_BG);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(COL_DIM, COL_BG);
    uiDrawString(tft, timeText, SCREEN_W - 8, titleY, hFont);
    tft.setTextDatum(TL_DATUM);
    lastHeaderTimeText = timeText;
  }
}

void getFooterDotRect(int pageIdx, int& x, int& y, int& w, int& h) {
  int count = cfg.pageCount;
  int spacing = SCREEN_W / count;
  x = pageIdx * spacing;
  y = SCREEN_H - FOOTER_H;
  w = spacing;
  h = FOOTER_H;
}

void drawFooter(bool force) {
  if (!force) return; // footer only changes on page switch, always full-redraw

  tft.fillRect(0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H, COL_BG);
  tft.drawFastHLine(0, SCREEN_H - FOOTER_H, SCREEN_W, COL_STROKE);

  for (int i = 0; i < cfg.pageCount; i++) {
    int x, y, w, h;
    getFooterDotRect(i, x, y, w, h);
    int cx = x + w / 2;
    int cy = y + h / 2;

    if (i == currentPageIndex) {
      tft.fillCircle(cx, cy, 5, COL_ACCENT);
    } else {
      tft.drawCircle(cx, cy, 4, COL_DIM);
    }
  }
}

// =========================================================
// PAGE DRAW / UPDATE
// =========================================================
void drawGridBackground() {
  PageConfig& pg = cfg.pages[currentPageIndex];
  int slotOf[MAX_TILES];
  layoutPageTiles(pg, slotOf);

  tft.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H - FOOTER_H, COL_BG);

  for (int i = 0; i < pg.tileCount; i++) {
    if (slotOf[i] < 0) continue;
    bool wide = (pg.tiles[i].size == 2);
    int x, y, w, h;
    getSlotRect(slotOf[i], wide, x, y, w, h);
    tft.fillRoundRect(x, y, w, h, 14, COL_PANEL);
    if (showTileBorder) tft.drawRoundRect(x, y, w, h, 14, COL_STROKE);
  }
}

// Shared between drawing and touch hit-testing so the two can never drift
// out of sync with each other.
const int STATUS_INFO_Y0 = HEADER_H + 14;                          // 48
const int STATUS_ROW_H = 22;
const int STATUS_THEME_BTN_X = 12, STATUS_THEME_BTN_W = 216, STATUS_BTN_H = 34;
const int STATUS_THEME_BTN_Y = STATUS_INFO_Y0 + 3 * STATUS_ROW_H + 24;
const int STATUS_REBOOT_BTN_Y = STATUS_THEME_BTN_Y + STATUS_BTN_H + 10;

void drawStatusPageFull() {
  PageConfig& pg = cfg.pages[currentPageIndex];
  tft.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H - FOOTER_H, COL_BG);

  int rowFont = fontStatusRow();
  int btnFont = fontStatusButton();

  int y = STATUS_INFO_Y0;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  uiDrawString(tft, "WiFi", 12, y, rowFont);
  tft.setTextColor(COL_TEXT, COL_BG);
  uiDrawString(tft, WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected", 80, y, rowFont);
  y += STATUS_ROW_H;

  tft.setTextColor(COL_DIM, COL_BG);
  uiDrawString(tft, "IP", 12, y, rowFont);
  tft.setTextColor(COL_TEXT, COL_BG);
  uiDrawString(tft, WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "-", 80, y, rowFont);
  y += STATUS_ROW_H;

  tft.setTextColor(COL_DIM, COL_BG);
  uiDrawString(tft, "Signal", 12, y, rowFont);
  tft.setTextColor(COL_TEXT, COL_BG);
  uiDrawString(tft, WiFi.status() == WL_CONNECTED ? (String(WiFi.RSSI()) + " dBm") : "-", 80, y, rowFont);
  y += STATUS_ROW_H;

  tft.setTextColor(COL_DIM, COL_BG);
  uiDrawString(tft, "Host", 12, y, rowFont);
  tft.setTextColor(COL_TEXT, COL_BG);
  uiDrawString(tft, String(cfg.deviceName) + ".local", 80, y, rowFont);

  // Theme toggle button
  tft.fillRoundRect(STATUS_THEME_BTN_X, STATUS_THEME_BTN_Y, STATUS_THEME_BTN_W, STATUS_BTN_H, 14, COL_PANEL);
  if (showTileBorder) tft.drawRoundRect(STATUS_THEME_BTN_X, STATUS_THEME_BTN_Y, STATUS_THEME_BTN_W, STATUS_BTN_H, 14, COL_STROKE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_TEXT, COL_PANEL);
  uiDrawString(tft, cfg.darkTheme ? "Dark Mode: ON" : "Light Mode: ON",
                 STATUS_THEME_BTN_X + STATUS_THEME_BTN_W / 2, STATUS_THEME_BTN_Y + STATUS_BTN_H / 2, btnFont);

  // Reboot button
  uint16_t rbColor = rebootArmed ? fixColor565(TFT_RED) : COL_PANEL;
  tft.fillRoundRect(STATUS_THEME_BTN_X, STATUS_REBOOT_BTN_Y, STATUS_THEME_BTN_W, STATUS_BTN_H, 14, rbColor);
  if (showTileBorder) tft.drawRoundRect(STATUS_THEME_BTN_X, STATUS_REBOOT_BTN_Y, STATUS_THEME_BTN_W, STATUS_BTN_H, 14, COL_STROKE);
  tft.setTextColor(rebootArmed ? fixColor565(TFT_WHITE) : COL_TEXT, rbColor);
  uiDrawString(tft, rebootArmed ? "Tap Again" : "Reboot Device",
                 STATUS_THEME_BTN_X + STATUS_THEME_BTN_W / 2, STATUS_REBOOT_BTN_Y + STATUS_BTN_H / 2, btnFont);
  tft.setTextDatum(TL_DATUM);
}

void drawForecastPageFull() {
  tft.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H - FOOTER_H, COL_BG);

  if (!weatherKnown) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_DIM, COL_BG);
    uiDrawString(tft, WiFi.status() == WL_CONNECTED ? "Fetching forecast..." : "No WiFi connection",
                   SCREEN_W / 2, SCREEN_H / 2, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  // "Current conditions" hero row, matching the layout of Home Assistant's
  // own weather card: icon + condition + area name on the left, current
  // temp + today's high/low on the right.
  const int heroTop = HEADER_H;
  const int heroIconCx = 40;
  const int heroIconCy = heroTop + 38;

  drawWeatherIcon(tft, heroIconCx, heroIconCy, weatherCurrentCode, 1.5f, weatherIsDay, COL_BG);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  uiDrawString(tft, weatherCodeLabel(weatherCurrentCode), 68, heroTop + 12, fontHeader());
  tft.setTextColor(COL_DIM, COL_BG);
  uiDrawString(tft, cfg.pages[0].name, 68, heroTop + 40, fontStatusRow());

  String curTemp = String((int)roundf(weatherCurrentTemp));
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  int curTempW = tft.textWidth(curTemp, 4);
  uiDrawString(tft, curTemp, SCREEN_W - 8, heroTop + 10, 4);
  tft.drawCircle(SCREEN_W - 8 - curTempW - 6, heroTop + 14, 2, COL_TEXT);

  String hiLo = String((int)roundf(forecast[0].tempMax)) + "/" + String((int)roundf(forecast[0].tempMin));
  tft.setTextColor(COL_DIM, COL_BG);
  uiDrawString(tft, hiLo, SCREEN_W - 8, heroTop + 44, fontStatusRow());

  int dividerY = heroTop + 76;
  tft.drawFastHLine(8, dividerY, SCREEN_W - 16, COL_STROKE);

  int colW = SCREEN_W / 5;
  int bodyTop = dividerY + 10;
  int rf = fontStatusRow();

  tft.setTextDatum(MC_DATUM);

  for (int i = 0; i < 5; i++) {
    int cx = i * colW + colW / 2;

    if (i > 0) tft.drawFastVLine(i * colW, bodyTop + 8, 140, COL_STROKE);

    tft.setTextColor(COL_DIM, COL_BG);
    uiDrawString(tft, forecast[i].dayLabel, cx, bodyTop + 10, rf);

    drawWeatherIcon(tft, cx, bodyTop + 46, forecast[i].code, 1.0f, true, COL_BG);

    tft.setTextColor(COL_TEXT, COL_BG);
    String hi = String((int)roundf(forecast[i].tempMax));
    int hiW = tft.textWidth(hi, rf);
    uiDrawString(tft, hi, cx, bodyTop + 82, rf);
    tft.drawCircle(cx + hiW / 2 + 4, bodyTop + 76, 2, COL_TEXT);

    tft.setTextColor(COL_DIM, COL_BG);
    String lo = String((int)roundf(forecast[i].tempMin));
    int loW = tft.textWidth(lo, rf);
    uiDrawString(tft, lo, cx, bodyTop + 106, rf);
    tft.drawCircle(cx + loW / 2 + 4, bodyTop + 100, 2, COL_DIM);
  }

  tft.setTextDatum(TL_DATUM);
}

// Shared with handleTimersPageTap() so drawing and touch hit-testing
// can never drift out of sync with each other.
const int TIMER_CANCEL_BTN_W = 140, TIMER_CANCEL_BTN_H = 40;
const int TIMER_CANCEL_BTN_X = (SCREEN_W - TIMER_CANCEL_BTN_W) / 2;
const int TIMER_CANCEL_BTN_Y = HEADER_H + 150;

// The Cancel button's screen position genuinely overlaps where the preset
// grid buttons sit (there's no free space to relocate it to on a screen
// this small) - so right after Cancel swaps the view back to the preset
// grid, a touch controller contact-bounce (or a fast human re-tap before
// they've registered the view changed) can land on a now-live preset
// button at that same spot. A time-based touch debounce alone wasn't
// enough, so this makes the preset grid itself briefly inert immediately
// after it appears, regardless of what's causing the stray touch.
unsigned long presetGridShownMs = 0;
const unsigned long PRESET_GRID_GRACE_MS = 500;

void drawTimersPageFull() {
  tft.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H - FOOTER_H, COL_BG);

  if (timerRunning) {
    unsigned long mins = timerRemainingSec / 60UL;
    unsigned long secs = timerRemainingSec % 60UL;
    char buf[8];
    snprintf(buf, sizeof(buf), "%lu:%02lu", mins, secs);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_DIM, COL_BG);
    uiDrawString(tft, "Time Remaining", SCREEN_W / 2, HEADER_H + 40, 2);
    tft.setTextColor(COL_TEXT, COL_BG);
    uiDrawString(tft, buf, SCREEN_W / 2, HEADER_H + 90, 4);

    tft.fillRoundRect(TIMER_CANCEL_BTN_X, TIMER_CANCEL_BTN_Y, TIMER_CANCEL_BTN_W, TIMER_CANCEL_BTN_H, 14, COL_PANEL);
    if (showTileBorder) tft.drawRoundRect(TIMER_CANCEL_BTN_X, TIMER_CANCEL_BTN_Y, TIMER_CANCEL_BTN_W, TIMER_CANCEL_BTN_H, 14, COL_STROKE);
    tft.setTextColor(COL_TEXT, COL_PANEL);
    uiDrawString(tft, "Cancel", TIMER_CANCEL_BTN_X + TIMER_CANCEL_BTN_W / 2, TIMER_CANCEL_BTN_Y + TIMER_CANCEL_BTN_H / 2, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  // Preset buttons fill grid slots 0-4; the "flash lights" checkbox
  // takes slot 5, reusing the same 2x3 tile grid geometry as other pages.
  presetGridShownMs = millis();
  for (int i = 0; i < 5; i++) {
    int x, y, w, h;
    getSlotRect(i, false, x, y, w, h);
    tft.fillRoundRect(x, y, w, h, 14, COL_PANEL);
    if (showTileBorder) tft.drawRoundRect(x, y, w, h, 14, COL_STROKE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_TEXT, COL_PANEL);
    String label = formatPresetLabel(cfg.timerPresetSec[i]);
    uiDrawString(tft, label, x + w / 2, y + h / 2, fontTile());
  }

  int cx, cy, cw, ch;
  getSlotRect(5, false, cx, cy, cw, ch);
  tft.fillRoundRect(cx, cy, cw, ch, 14, COL_PANEL);
  if (showTileBorder) tft.drawRoundRect(cx, cy, cw, ch, 14, COL_STROKE);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_DIM, COL_PANEL);
  uiDrawString(tft, "Flash Lights", cx + cw / 2, cy + 6, 1);

  int boxSize = 24;
  int boxX = cx + cw / 2 - boxSize / 2;
  int boxY = cy + ch / 2 - boxSize / 2 + 6;
  tft.drawRoundRect(boxX, boxY, boxSize, boxSize, 4, COL_STROKE);
  if (timerFlashOnExpire) {
    tft.fillRoundRect(boxX + 4, boxY + 4, boxSize - 8, boxSize - 8, 2, COL_ACCENT);
  }
  tft.setTextDatum(TL_DATUM);
}

void updateGridTiles(bool force) {
  PageConfig& pg = cfg.pages[currentPageIndex];
  int slotOf[MAX_TILES];
  layoutPageTiles(pg, slotOf);

  for (int i = 0; i < pg.tileCount; i++) {
    bool wide = (pg.tiles[i].size == 2);
    if (sliderActive && touchTileIndex == i) {
      drawSliderSprite(i, slotOf[i], wide, sliderPercent, force);
    } else {
      drawTileSprite(i, slotOf[i], wide, force);
    }
  }
}

void drawCurrentPageFull() {
  PageConfig& pg = cfg.pages[currentPageIndex];
  drawHeader(true);
  drawFooter(true);

  if (strcmp(pg.type, "status") == 0) {
    drawStatusPageFull();
  } else if (strcmp(pg.type, "forecast") == 0) {
    drawForecastPageFull();
  } else if (strcmp(pg.type, "timers") == 0) {
    drawTimersPageFull();
  } else {
    drawGridBackground();
    updateGridTiles(true);
  }

  pageDirty = false;
}

// Timers page isn't tile-based, so its countdown needs its own
// lightweight per-tick partial update (redraw just the mm:ss text,
// not the whole page) while a timer is running and that page is shown.
void updateTimersCountdownText() {
  if (!timerRunning) return;
  unsigned long mins = timerRemainingSec / 60UL;
  unsigned long secs = timerRemainingSec % 60UL;
  char buf[8];
  snprintf(buf, sizeof(buf), "%lu:%02lu", mins, secs);
  String text(buf);
  if (text == lastTimersCountdownText) return;
  lastTimersCountdownText = text;

  int textY = HEADER_H + 90;
  tft.fillRect(0, textY - 16, SCREEN_W, 32, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  uiDrawString(tft, text, SCREEN_W / 2, textY, 4);
  tft.setTextDatum(TL_DATUM);
}

void updateCurrentPageDynamic() {
  drawHeader(false);
  PageConfig& pg = cfg.pages[currentPageIndex];
  if (strcmp(pg.type, "home") == 0 || strcmp(pg.type, "area") == 0) {
    updateGridTiles(false);
  } else if (strcmp(pg.type, "timers") == 0) {
    updateTimersCountdownText();
  }
}

// =========================================================
// POLLING
// =========================================================
void pollCurrentPageTiles(bool force) {
  PageConfig& pg = cfg.pages[currentPageIndex];
  if (strcmp(pg.type, "home") != 0 && strcmp(pg.type, "area") != 0) return;
  if (!haConfigured() || WiFi.status() != WL_CONNECTED) return;

  for (int i = 0; i < pg.tileCount; i++) {
    TileConfig& tl = pg.tiles[i];
    if (strlen(tl.entityId) == 0) continue;
    if (strcmp(tl.type, "light") != 0 && strcmp(tl.type, "switch") != 0 && strcmp(tl.type, "sensor") != 0) continue;
    if (sliderActive && touchTileIndex == i) continue; // don't stomp an active drag

    TileRuntime& rt = tileRuntime[currentPageIndex][i];
    if (haFetchEntityState(tl.entityId, rt)) {
      rt.cacheKey = ""; // force a redraw
    }
  }
}

// =========================================================
// TOUCH: TAP / LONG-PRESS SLIDER / SWIPE
// =========================================================
int hitTestTile(int x, int y) {
  PageConfig& pg = cfg.pages[currentPageIndex];
  int slotOf[MAX_TILES];
  layoutPageTiles(pg, slotOf);

  // Extend each tile's touch target a few px into the gaps between tiles.
  // Those gaps are only ~8px wide - without this, a press near a tile's
  // edge (exactly where you'd grab to start a drag) can miss the tile
  // entirely and get treated as a swipe instead.
  const int HIT_PAD = 6;
  for (int i = 0; i < pg.tileCount; i++) {
    if (slotOf[i] < 0) continue;
    bool wide = (pg.tiles[i].size == 2);
    int tx, ty, tw, th;
    getSlotRect(slotOf[i], wide, tx, ty, tw, th);
    if (x >= tx - HIT_PAD && x < tx + tw + HIT_PAD && y >= ty - HIT_PAD && y < ty + th + HIT_PAD) return i;
  }
  return -1;
}

void navigateToPageType(const char* type) {
  for (int i = 0; i < cfg.pageCount; i++) {
    if (strcmp(cfg.pages[i].type, type) == 0) {
      currentPageIndex = i;
      pageDirty = true;
      return;
    }
  }
}

void handleTileTap(int tileIdx) {
  PageConfig& pg = cfg.pages[currentPageIndex];
  TileConfig& tl = pg.tiles[tileIdx];

  // Weather/Timer tiles aren't toggleable - tapping jumps to their page.
  if (strcmp(tl.type, "weather") == 0) {
    navigateToPageType("forecast");
    return;
  }
  if (strcmp(tl.type, "timer") == 0) {
    navigateToPageType("timers");
    return;
  }

  TileRuntime& rt = tileRuntime[currentPageIndex][tileIdx];

  if (strlen(tl.entityId) == 0 || !haConfigured()) return;
  if (strcmp(tl.type, "sensor") == 0) return; // read-only

  bool newState = !(rt.known && rt.on);
  rt.on = newState;
  rt.known = true;
  rt.cacheKey = "";

  int slotOf[MAX_TILES];
  layoutPageTiles(pg, slotOf);
  drawTileSprite(tileIdx, slotOf[tileIdx], tl.size == 2, true);

  haSendCommand(tl.entityId, newState);
}

void handleStatusPageTap(int x, int y) {
  bool inButtonX = (x >= STATUS_THEME_BTN_X && x < STATUS_THEME_BTN_X + STATUS_THEME_BTN_W);

  if (inButtonX && y >= STATUS_THEME_BTN_Y && y < STATUS_THEME_BTN_Y + STATUS_BTN_H) {
    cfg.darkTheme = !cfg.darkTheme;
    applyTheme(cfg.darkTheme);
    saveConfig();
    pageDirty = true;
    return;
  }

  if (inButtonX && y >= STATUS_REBOOT_BTN_Y && y < STATUS_REBOOT_BTN_Y + STATUS_BTN_H) {
    if (rebootArmed) {
      ESP.restart();
    } else {
      rebootArmed = true;
      rebootArmedMs = millis();
      pageDirty = true;
    }
  }
}

void handleTimersPageTap(int x, int y) {
  if (timerRunning) {
    if (x >= TIMER_CANCEL_BTN_X && x < TIMER_CANCEL_BTN_X + TIMER_CANCEL_BTN_W &&
        y >= TIMER_CANCEL_BTN_Y && y < TIMER_CANCEL_BTN_Y + TIMER_CANCEL_BTN_H) {
      stopTimer();
    }
    return;
  }

  if (millis() - presetGridShownMs < PRESET_GRID_GRACE_MS) return;

  for (int i = 0; i < 5; i++) {
    int sx, sy, sw, sh;
    getSlotRect(i, false, sx, sy, sw, sh);
    if (x >= sx && x < sx + sw && y >= sy && y < sy + sh) {
      startTimer(cfg.timerPresetSec[i]);
      return;
    }
  }

  int cx, cy, cw, ch;
  getSlotRect(5, false, cx, cy, cw, ch);
  if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) {
    timerFlashOnExpire = !timerFlashOnExpire;
    pageDirty = true;
  }
}

// Shared with handleTimerDialogTouch() so drawing and touch hit-testing
// can never drift out of sync with each other.
const int DIALOG_W = 200, DIALOG_H = 110;
const int DIALOG_X = (SCREEN_W - DIALOG_W) / 2;
const int DIALOG_Y = (SCREEN_H - DIALOG_H) / 2;
const int DIALOG_BTN_W = 84, DIALOG_BTN_H = 34;
const int DIALOG_BTN_Y = DIALOG_Y + DIALOG_H - DIALOG_BTN_H - 12;
const int DIALOG_STOP_X = DIALOG_X + 10;
const int DIALOG_RESTART_X = DIALOG_X + DIALOG_W - DIALOG_BTN_W - 10;

void drawTimerExpiredDialog() {
  tft.fillRoundRect(DIALOG_X, DIALOG_Y, DIALOG_W, DIALOG_H, 14, COL_PANEL);
  if (showTileBorder) tft.drawRoundRect(DIALOG_X, DIALOG_Y, DIALOG_W, DIALOG_H, 14, COL_STROKE);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_TEXT, COL_PANEL);
  uiDrawString(tft, "Timer Expired", DIALOG_X + DIALOG_W / 2, DIALOG_Y + 26, 2);

  tft.fillRoundRect(DIALOG_STOP_X, DIALOG_BTN_Y, DIALOG_BTN_W, DIALOG_BTN_H, 8, COL_PANEL_ALT);
  tft.drawRoundRect(DIALOG_STOP_X, DIALOG_BTN_Y, DIALOG_BTN_W, DIALOG_BTN_H, 8, COL_STROKE);
  tft.setTextColor(COL_TEXT, COL_PANEL_ALT);
  uiDrawString(tft, "Stop", DIALOG_STOP_X + DIALOG_BTN_W / 2, DIALOG_BTN_Y + DIALOG_BTN_H / 2, 2);

  tft.fillRoundRect(DIALOG_RESTART_X, DIALOG_BTN_Y, DIALOG_BTN_W, DIALOG_BTN_H, 8, COL_ACCENT);
  tft.drawRoundRect(DIALOG_RESTART_X, DIALOG_BTN_Y, DIALOG_BTN_W, DIALOG_BTN_H, 8, COL_ACCENT);
  tft.setTextColor(COL_BG, COL_ACCENT);
  uiDrawString(tft, "Restart", DIALOG_RESTART_X + DIALOG_BTN_W / 2, DIALOG_BTN_Y + DIALOG_BTN_H / 2, 2);

  tft.setTextDatum(TL_DATUM);
}

// Fully separate from handleContinuousTouch() - while the dialog is
// open it's the ONLY touch handler running (see loop()), so it needs
// its own independent press-edge tracking rather than sharing state
// with the main handler.
void handleTimerDialogTouch() {
  int x = 0, y = 0;
  bool down = readTouchXY(x, y);
  static bool wasDownDialog = false;

  if (down && !wasDownDialog) {
    if (x >= DIALOG_STOP_X && x < DIALOG_STOP_X + DIALOG_BTN_W &&
        y >= DIALOG_BTN_Y && y < DIALOG_BTN_Y + DIALOG_BTN_H) {
      stopTimer();
    } else if (x >= DIALOG_RESTART_X && x < DIALOG_RESTART_X + DIALOG_BTN_W &&
               y >= DIALOG_BTN_Y && y < DIALOG_BTN_Y + DIALOG_BTN_H) {
      restartTimer();
    }
  }
  wasDownDialog = down;
}

void handleContinuousTouch() {
  int x = 0, y = 0;
  bool down = readTouchXY(x, y);
  static bool wasDown = false;

  PageConfig& pg = cfg.pages[currentPageIndex];
  bool isGridPage = (strcmp(pg.type, "home") == 0 || strcmp(pg.type, "area") == 0);

  if (down && !wasDown) {
    if (millis() - lastTouchReleaseMs < TOUCH_RELEASE_DEBOUNCE_MS) {
      // Likely contact bounce right after a previous release - ignore
      // it rather than let it start tracking a brand new press.
      wasDown = down;
      return;
    }

    int hit = isGridPage ? hitTestTile(x, y) : -1;
    if (hit >= 0) {
      touchTileIndex = hit;
      touchDown = true;
      touchMoved = false;
      sliderActive = false;
      touchStartMs = millis();
      touchStartX = x;
      touchStartY = y;
      swipeCandidate = false;
    } else {
      // Empty grid space (or a non-grid page) - candidate for a swipe.
      touchDown = false;
      touchTileIndex = -1;
      swipeCandidate = true;
      swipeStartX = x;
      swipeStartY = y;
    }
  } else if (down && wasDown && touchDown && touchTileIndex >= 0) {
    int idx = touchTileIndex;
    TileConfig& tl = pg.tiles[idx];
    bool isLight = (strcmp(tl.type, "light") == 0);

    if (!sliderActive) {
      int dx = abs(x - touchStartX);
      int dy = abs(y - touchStartY);
      if (dx > MOVE_TOLERANCE || dy > MOVE_TOLERANCE) touchMoved = true;

      if (isLight && !touchMoved && (millis() - touchStartMs) >= LONG_PRESS_MS) {
        sliderActive = true;
        TileRuntime& rt = tileRuntime[currentPageIndex][idx];
        sliderPercent = constrain(rt.brightnessPct, 0, 100);
        rt.cacheKey = "";

        int slotOf[MAX_TILES];
        layoutPageTiles(pg, slotOf);
        drawSliderSprite(idx, slotOf[idx], tl.size == 2, sliderPercent, true);
        lastSliderSendMs = 0;
      }
    } else {
      int slotOf[MAX_TILES];
      layoutPageTiles(pg, slotOf);
      int sx, sy, sw, sh;
      getSlotRect(slotOf[idx], tl.size == 2, sx, sy, sw, sh);
      int trackTopAbs = sy + 18;
      int trackBottomAbs = sy + sh - 8;
      int clampedY = constrain(y, trackTopAbs, trackBottomAbs);
      int span = max(1, trackBottomAbs - trackTopAbs);
      int pct = 100 - ((clampedY - trackTopAbs) * 100) / span;
      pct = constrain(pct, 0, 100);

      if (pct != sliderPercent) {
        sliderPercent = pct;
        drawSliderSprite(idx, slotOf[idx], tl.size == 2, sliderPercent, true);

        unsigned long now = millis();
        if (now - lastSliderSendMs >= SLIDER_SEND_INTERVAL_MS) {
          lastSliderSendMs = now;
          haSendBrightness(tl.entityId, sliderPercent);
          TileRuntime& rt = tileRuntime[currentPageIndex][idx];
          rt.brightnessPct = sliderPercent;
          rt.on = sliderPercent > 0;
          rt.known = true;
        }
      }
    }
  } else if (!down && wasDown) {
    lastTouchReleaseMs = millis();

    if (touchDown && touchTileIndex >= 0) {
      int idx = touchTileIndex;
      TileConfig& tl = pg.tiles[idx];

      if (sliderActive) {
        haSendBrightness(tl.entityId, sliderPercent);
        TileRuntime& rt = tileRuntime[currentPageIndex][idx];
        rt.brightnessPct = sliderPercent;
        rt.on = sliderPercent > 0;
        rt.known = true;
        rt.cacheKey = "";

        int slotOf[MAX_TILES];
        layoutPageTiles(pg, slotOf);
        drawTileSprite(idx, slotOf[idx], tl.size == 2, true);
      } else if (!touchMoved) {
        handleTileTap(idx);
      }

      touchDown = false;
      touchTileIndex = -1;
      sliderActive = false;
      touchMoved = false;
    } else if (swipeCandidate) {
      int dx = x - swipeStartX;
      int dy = abs(y - swipeStartY);

      if (abs(dx) >= SWIPE_THRESHOLD_X && dy < 40) {
        if (dx < 0 && currentPageIndex < cfg.pageCount - 1) currentPageIndex++;
        else if (dx > 0 && currentPageIndex > 0) currentPageIndex--;
        pageDirty = true;
      } else if (swipeStartY >= SCREEN_H - FOOTER_H) {
        // Footer dot tap takes priority over any page-specific handling.
        for (int i = 0; i < cfg.pageCount; i++) {
          int fx, fy, fw, fh;
          getFooterDotRect(i, fx, fy, fw, fh);
          if (swipeStartX >= fx && swipeStartX < fx + fw) {
            if (i != currentPageIndex) {
              currentPageIndex = i;
              pageDirty = true;
            }
            break;
          }
        }
      } else if (strcmp(pg.type, "status") == 0) {
        handleStatusPageTap(swipeStartX, swipeStartY);
      } else if (strcmp(pg.type, "timers") == 0) {
        handleTimersPageTap(swipeStartX, swipeStartY);
      }

      swipeCandidate = false;
    }
  }

  wasDown = down;
}

// =========================================================
// WEB SERVER
// =========================================================
WebServer server(80);

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

String pageHeaderHtml(const String& title) {
  const WebFontEntry& font = findWebFont("poppins"); // font picker removed - Poppins is the settled choice

  String h;
  h += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>" + htmlEscape(title) + "</title>";
  if (font.googleFontsParam) {
    h += "<link rel='preconnect' href='https://fonts.googleapis.com'>";
    h += "<link rel='preconnect' href='https://fonts.gstatic.com' crossorigin>";
    h += "<link href='https://fonts.googleapis.com/css2?family=" + String(font.googleFontsParam) + "&display=swap' rel='stylesheet'>";
  }
  h += "<style>";
  h += ":root{--bg:#0e1014;--panel:#181b21;--panel2:#20242c;--border:#2a2f3a;--text:#f2f4f6;--dim:#95a0af;--accent:#4fc3f7;--font:" + String(font.cssFamily) + ";}";
  h += "*{box-sizing:border-box;}";
  h += "body{font-family:var(--font);background:var(--bg);color:var(--text);margin:0;}";
  h += ".wrap{max-width:480px;margin:0 auto;padding:20px 18px 60px;}";
  h += "h1{font-size:22px;margin:4px 0 18px;letter-spacing:0.03em;}";
  h += "h2{font-size:13px;margin:26px 0 8px;color:var(--dim);text-transform:uppercase;letter-spacing:0.08em;}";
  h += "a{color:var(--accent);text-decoration:none;} a:hover{text-decoration:underline;}";
  h += "label{display:block;font-size:13px;color:var(--dim);margin:10px 0 4px;}";
  h += ".check{display:flex;align-items:center;gap:8px;margin:14px 0 6px;}";
  h += ".grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px;}";
  h += ".check input{width:18px;height:18px;flex:none;margin:0;}";
  h += "input,select{padding:10px 12px;margin:0;width:100%;background:var(--panel2);color:var(--text);border:1px solid var(--border);border-radius:10px;font-size:14px;font-family:var(--font);}";
  h += "input:focus,select:focus{outline:none;border-color:var(--accent);}";
  h += "button{padding:9px 16px;margin:10px 6px 0 0;background:var(--panel2);color:var(--text);border:1px solid var(--border);border-radius:10px;font-size:13px;font-family:var(--font);cursor:pointer;}";
  h += "button.primary{background:var(--accent);color:#062230;border-color:var(--accent);font-weight:600;}";
  h += "button.danger{background:#3a1620;color:#ff8a9b;border-color:#5a2230;}";
  h += "button:disabled{opacity:0.35;cursor:default;}";
  h += ".row{background:var(--panel);border:1px solid var(--border);border-radius:14px;padding:14px 16px;margin:10px 0;}";
  h += ".row b{font-size:15px;}";
  h += ".muted{color:var(--dim);font-size:13px;}";
  h += "form{margin:0;}";
  h += "</style></head><body><div class='wrap'>";
  return h;
}

const String htmlFooter = "</div></body></html>";

String toUpperStr(String s) {
  s.toUpperCase();
  return s;
}

void handleRoot() {
  String h = pageHeaderHtml("HA Panel Settings");
  h += "<h1>" + htmlEscape(toUpperStr(cfg.deviceName)) + "</h1>";

  h += "<h2>Device</h2><form method='POST' action='/save-device'>";
  h += "<label>Device name (also used as .local hostname)</label>";
  h += "<input name='deviceName' value='" + htmlEscape(cfg.deviceName) + "'>";
  h += "<label>Area / room name (shown on this panel's Home screen)</label>";
  h += "<input name='areaName' value='" + htmlEscape(cfg.pages[0].name) + "' placeholder='e.g. Family Room'>";
  h += "<div class='check'><input type='checkbox' id='use12h' name='use12h'" + String(cfg.use12Hour ? " checked" : "") +
       "><label for='use12h' style='margin:0'>Use 12-hour clock (AM/PM)</label></div>";
  h += "<label>Time Zone</label><select name='timezone'>";
  for (int i = 0; i < TZ_TABLE_COUNT; i++) {
    h += "<option value='" + String(TZ_TABLE[i].key) + "'";
    if (String(cfg.timezone) == TZ_TABLE[i].key) h += " selected";
    h += ">" + String(TZ_TABLE[i].label) + "</option>";
  }
  h += "</select>";
  h += "<label>HA Panel Text Size</label><select name='uiFontSize'>";
  h += "<option value='0'" + String(cfg.uiFontSize == 0 ? " selected" : "") + ">Small</option>";
  h += "<option value='1'" + String(cfg.uiFontSize == 1 ? " selected" : "") + ">Medium</option>";
  h += "<option value='2'" + String(cfg.uiFontSize == 2 ? " selected" : "") + ">Large</option>";
  h += "</select>";
  h += "<label>HA Panel Typeface (experimental)</label><select name='uiTypeface'>";
  for (int i = 0; i < UI_TYPEFACES_COUNT; i++) {
    h += "<option value='" + String(UI_TYPEFACES[i].key) + "'";
    if (String(cfg.uiTypeface) == UI_TYPEFACES[i].key) h += " selected";
    h += ">" + String(UI_TYPEFACES[i].label) + "</option>";
  }
  h += "</select>";
  h += "<div class='check'><input type='checkbox' id='uiBoldText' name='uiBoldText'" + String(cfg.uiBoldText ? " checked" : "") +
       "><label for='uiBoldText' style='margin:0'>Bold / thicker stroke</label></div>";
  h += "<div class='muted' style='margin-top:6px;'>Classic is the built-in bitmap font this project has used all along - guaranteed to fit every screen. The other options are genuinely different typefaces, but layouts were pixel-tuned against Classic's metrics, so text may sit differently or clip in spots until we see how it actually renders and adjust.</div>";

  h += "<div class='check'><input type='checkbox' id='weatherAuto' name='weatherAuto'" + String(cfg.weatherLocationAuto ? " checked" : "") +
       "><label for='weatherAuto' style='margin:0'>Match weather location to time zone</label></div>";
  h += "<label>Weather Location</label>";
  h += "<input name='weatherName' value='" + htmlEscape(cfg.weatherLocationName) + "' placeholder='e.g. Seattle, WA'>";
  h += "<div class='grid2'>";
  h += "<div><label>Latitude</label><input name='weatherLat' value='" + String(cfg.weatherLat, 4) + "'></div>";
  h += "<div><label>Longitude</label><input name='weatherLon' value='" + String(cfg.weatherLon, 4) + "'></div>";
  h += "</div>";
  h += "<div class='muted' style='margin-top:6px;'>When matched to time zone, location updates automatically and the fields above are ignored. Uncheck to set an exact location.</div>";

  h += "<label>Home Assistant URL</label>";
  h += "<input name='haUrl' value='" + htmlEscape(cfg.haUrl) + "'>";
  h += "<label>Long-lived access token</label>";
  h += "<input type='password' name='haToken' placeholder='" +
       String(strlen(cfg.haToken) > 0 ? "Saved (leave blank to keep it)" : "Paste your HA token") + "'>";
  h += "<button class='primary' type='submit'>Save</button></form>";

  h += "<h2>Timer Light Flash</h2><form method='POST' action='/save-flash'>";
  h += "<label>Pulse rate (ms between on/off)</label><input name='flashRate' value='" + String(cfg.flashPulseRateMs) + "'>";
  h += "<label>Pulse count (full on/off cycles)</label><input name='flashCount' value='" + String(cfg.flashPulseCount) + "'>";
  h += "<label>Dim-phase brightness (%) - the \"on\" phase is always 100%</label>";
  h += "<input name='flashBrightness' value='" + String(cfg.flashBrightnessPct) + "'>";
  h += "<div class='muted' style='margin-top:6px;'>Applies to dimmable lights; switches without brightness just toggle fully on/off.</div>";
  h += "<label>Lights to flash (from the Home page)</label>";
  {
    String selectedIds = String(",") + cfg.flashLightIds + ",";
    PageConfig& homePage = cfg.pages[0];
    bool anyLights = false;
    for (int i = 0; i < homePage.tileCount; i++) {
      TileConfig& hlt = homePage.tiles[i];
      if ((strcmp(hlt.type, "light") == 0 || strcmp(hlt.type, "switch") == 0) && strlen(hlt.entityId) > 0) {
        anyLights = true;
        String eid = hlt.entityId;
        String needle = String(",") + eid + ",";
        bool checked = selectedIds.indexOf(needle) >= 0;
        h += "<div class='check'><input type='checkbox' name='flashLight' value='" + htmlEscape(eid) + "'" +
             String(checked ? " checked" : "") + "><label style='margin:0'>" + htmlEscape(hlt.label) + "</label></div>";
      }
    }
    if (!anyLights) {
      h += "<div class='muted'>Add light or switch tiles to the Home page first.</div>";
    }
  }
  h += "<button class='primary' type='submit'>Save</button></form>";

  h += "<h2>Timer Presets</h2><form method='POST' action='/save-timers'>";
  for (int i = 0; i < 5; i++) {
    int mins = cfg.timerPresetSec[i] / 60;
    int secs = cfg.timerPresetSec[i] % 60;
    h += "<div class='grid2' style='margin-top:" + String(i == 0 ? 4 : 12) + "px;'>";
    h += "<div><label>Preset " + String(i + 1) + " - min</label>";
    h += "<input name='presetMin" + String(i) + "' value='" + String(mins) + "'></div>";
    h += "<div><label>Preset " + String(i + 1) + " - sec</label>";
    h += "<input name='presetSec" + String(i) + "' value='" + String(secs) + "'></div>";
    h += "</div>";
  }
  h += "<button class='primary' type='submit'>Save</button></form>";

  h += "<h2>Pages</h2>";
  for (int i = 0; i < cfg.pageCount; i++) {
    PageConfig& pg = cfg.pages[i];
    h += "<div class='row'><b>" + htmlEscape(pg.name) + "</b> <span class='muted'>(" + String(pg.type) + ")</span><br>";
    h += "<a href='/page?id=" + String(pg.id) + "'>Manage tiles</a> &nbsp; ";
    h += "<form style='display:inline' method='POST' action='/page/rename'>";
    h += "<input type='hidden' name='id' value='" + String(pg.id) + "'>";
    h += "<input style='width:120px;display:inline-block' name='name' value='" + htmlEscape(pg.name) + "'>";
    h += "<button type='submit'>Rename</button></form>";
    if (pg.deletable) {
      h += " <form style='display:inline' method='POST' action='/page/delete' onsubmit=\"return confirm('Delete this page and its tiles?');\">";
      h += "<input type='hidden' name='id' value='" + String(pg.id) + "'>";
      h += "<button class='danger' type='submit'>Delete</button></form>";
    }
    h += "</div>";
  }

  if (cfg.pageCount < MAX_PAGES) {
    h += "<form method='POST' action='/page/add'>";
    h += "<label>Add area page</label><input name='name' placeholder='e.g. Kitchen'>";
    h += "<button class='primary' type='submit'>Add page</button></form>";
  } else {
    h += "<p>Maximum of " + String(MAX_PAGES) + " pages reached.</p>";
  }

  h += "<h2>Backup</h2><a href='/export'>Download config backup</a>";
  h += "<form method='POST' action='/import' enctype='multipart/form-data' style='margin-top:12px;' onsubmit=\"return confirm('This replaces ALL current settings - device name, pages, tiles, everything. Continue?');\">";
  h += "<label>Restore from a backup file</label>";
  h += "<input type='file' name='configFile' accept='.json'>";
  h += "<button class='primary' type='submit'>Import &amp; Reboot</button></form>";

  h += htmlFooter;
  server.send(200, "text/html", h);
}

PageConfig* findPageById(const String& id) {
  for (int i = 0; i < cfg.pageCount; i++) {
    if (id == cfg.pages[i].id) return &cfg.pages[i];
  }
  return nullptr;
}

void handlePageManage() {
  if (!server.hasArg("id")) { server.sendHeader("Location", "/"); server.send(303); return; }
  PageConfig* pg = findPageById(server.arg("id"));
  if (!pg) { server.sendHeader("Location", "/"); server.send(303); return; }

  String h = pageHeaderHtml(pg->name);
  h += "<p><a href='/'>&larr; Back to settings</a></p>";
  h += "<h1>" + htmlEscape(pg->name) + " tiles</h1>";

  int used = 0;
  for (int i = 0; i < pg->tileCount; i++) used += pg->tiles[i].size;

  for (int i = 0; i < pg->tileCount; i++) {
    TileConfig& tl = pg->tiles[i];
    h += "<div class='row'>";
    h += "<b>" + htmlEscape(tl.label) + "</b> <span class='muted'>(" + String(tl.type) + (tl.size == 2 ? ", wide" : "") + ")</span><br>";
    h += "<span class='muted'>" + htmlEscape(tl.entityId) + "</span><br>";
    h += "<form style='display:inline' method='POST' action='/tile/move'>";
    h += "<input type='hidden' name='pageId' value='" + String(pg->id) + "'>";
    h += "<input type='hidden' name='index' value='" + String(i) + "'>";
    h += "<input type='hidden' name='dir' value='up'>";
    h += "<button type='submit'" + String(i == 0 ? " disabled" : "") + ">Up</button></form>";
    h += "<form style='display:inline' method='POST' action='/tile/move'>";
    h += "<input type='hidden' name='pageId' value='" + String(pg->id) + "'>";
    h += "<input type='hidden' name='index' value='" + String(i) + "'>";
    h += "<input type='hidden' name='dir' value='down'>";
    h += "<button type='submit'" + String(i == pg->tileCount - 1 ? " disabled" : "") + ">Down</button></form>";
    h += "<form style='display:inline' method='GET' action='/tile/edit'>";
    h += "<input type='hidden' name='pageId' value='" + String(pg->id) + "'>";
    h += "<input type='hidden' name='index' value='" + String(i) + "'>";
    h += "<button type='submit'>Edit</button></form>";
    h += "<form style='display:inline' method='POST' action='/tile/delete' onsubmit=\"return confirm('Remove this tile?');\">";
    h += "<input type='hidden' name='pageId' value='" + String(pg->id) + "'>";
    h += "<input type='hidden' name='index' value='" + String(i) + "'>";
    h += "<button class='danger' type='submit'>Remove</button></form>";
    h += "</div>";
  }

  h += "<p>" + String(used) + " / 6 grid cells used.</p>";

  if (used < 6) {
    h += "<h2>Add tile</h2><form method='POST' action='/tile/add'>";
    h += "<input type='hidden' name='pageId' value='" + String(pg->id) + "'>";
    h += "<label>Type</label><select name='type'>";
    h += "<option value='light'>Light (toggle + dim)</option>";
    h += "<option value='switch'>Switch (toggle only)</option>";
    h += "<option value='sensor'>Sensor (read-only)</option>";
    h += "<option value='weather'>Weather (icon + temperature)</option>";
    h += "<option value='timer'>Timer (countdown status)</option>";
    h += "</select>";
    h += "<label>Label</label><input name='label' placeholder='e.g. Bedside Lamp'>";
    h += "<label>Entity ID (not used for Weather/Timer tiles)</label><input name='entityId' placeholder='e.g. light.tobias_bedside'>";
    h += "<label>Size</label><select name='size'><option value='1'>1x1</option><option value='2'>1x2 (wide)</option></select>";
    h += "<button class='primary' type='submit'>Add tile</button></form>";
  } else {
    h += "<p>This page's grid is full.</p>";
  }

  h += htmlFooter;
  server.send(200, "text/html", h);
}

void handleTileEditForm() {
  if (!server.hasArg("pageId") || !server.hasArg("index")) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  PageConfig* pg = findPageById(server.arg("pageId"));
  int idx = server.arg("index").toInt();
  if (!pg || idx < 0 || idx >= pg->tileCount) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  TileConfig& tl = pg->tiles[idx];

  String h = pageHeaderHtml("Edit tile");
  h += "<p><a href='/page?id=" + String(pg->id) + "'>&larr; Back to " + htmlEscape(pg->name) + "</a></p>";
  h += "<h1>Edit tile</h1>";
  h += "<form method='POST' action='/tile/update'>";
  h += "<input type='hidden' name='pageId' value='" + String(pg->id) + "'>";
  h += "<input type='hidden' name='index' value='" + String(idx) + "'>";

  h += "<label>Type</label><select name='type'>";
  const char* types[5] = {"light", "switch", "sensor", "weather", "timer"};
  const char* typeLabels[5] = {"Light (toggle + dim)", "Switch (toggle only)", "Sensor (read-only)", "Weather (icon + temperature)", "Timer (countdown status)"};
  for (int i = 0; i < 5; i++) {
    h += "<option value='" + String(types[i]) + "'";
    if (strcmp(tl.type, types[i]) == 0) h += " selected";
    h += ">" + String(typeLabels[i]) + "</option>";
  }
  h += "</select>";

  h += "<label>Label</label><input name='label' value='" + htmlEscape(tl.label) + "'>";
  h += "<label>Entity ID (not used for Weather/Timer tiles)</label><input name='entityId' value='" + htmlEscape(tl.entityId) + "'>";

  h += "<label>Size</label><select name='size'>";
  h += "<option value='1'" + String(tl.size == 1 ? " selected" : "") + ">1x1</option>";
  h += "<option value='2'" + String(tl.size == 2 ? " selected" : "") + ">1x2 (wide)</option>";
  h += "</select>";

  h += "<button class='primary' type='submit'>Save changes</button></form>";

  h += htmlFooter;
  server.send(200, "text/html", h);
}

void handleTileUpdate() {
  if (server.hasArg("pageId") && server.hasArg("index")) {
    PageConfig* pg = findPageById(server.arg("pageId"));
    int idx = server.arg("index").toInt();

    if (pg && idx >= 0 && idx < pg->tileCount) {
      TileConfig& tl = pg->tiles[idx];

      int newSize = server.hasArg("size") ? server.arg("size").toInt() : tl.size;
      if (newSize != 2) newSize = 1;

      // Check the new size still fits the page's 6-cell grid (other tiles unchanged).
      int usedByOthers = 0;
      for (int i = 0; i < pg->tileCount; i++) {
        if (i != idx) usedByOthers += pg->tiles[i].size;
      }

      if (usedByOthers + newSize <= 6) {
        String type = server.hasArg("type") ? server.arg("type") : tl.type;
        String label = server.hasArg("label") ? server.arg("label") : tl.label;
        String entityId = server.hasArg("entityId") ? server.arg("entityId") : tl.entityId;
        label.trim();
        entityId.trim();
        if (label.length() == 0) label = entityId.length() > 0 ? entityId : "Tile";

        strlcpy(tl.type, type.c_str(), sizeof(tl.type));
        strlcpy(tl.label, label.c_str(), sizeof(tl.label));
        strlcpy(tl.entityId, entityId.c_str(), sizeof(tl.entityId));
        tl.size = (uint8_t)newSize;

        // Runtime state (on/off/brightness/cache) may no longer apply if the
        // entity or type changed - clear it so the tile refetches cleanly.
        int pageIdx = (int)(pg - cfg.pages);
        tileRuntime[pageIdx][idx] = TileRuntime();

        saveConfig();
        pageDirty = true;
      }
    }
  }
  server.sendHeader("Location", "/page?id=" + server.arg("pageId"));
  server.send(303);
}

void handleSaveDevice() {
  String newName = server.hasArg("deviceName") ? server.arg("deviceName") : cfg.deviceName;
  String newUrl = server.hasArg("haUrl") ? server.arg("haUrl") : cfg.haUrl;
  String newToken = server.hasArg("haToken") ? server.arg("haToken") : "";
  String newAreaName = server.hasArg("areaName") ? server.arg("areaName") : cfg.pages[0].name;

  newName.trim();
  if (newName.length() == 0) newName = DEVICE_NAME_DEFAULT;
  newUrl.trim();
  while (newUrl.endsWith("/")) newUrl.remove(newUrl.length() - 1);
  newToken.trim();
  newAreaName.trim();
  if (newAreaName.length() == 0) newAreaName = "Home";

  strlcpy(cfg.deviceName, newName.c_str(), sizeof(cfg.deviceName));
  strlcpy(cfg.haUrl, newUrl.c_str(), sizeof(cfg.haUrl));
  if (newToken.length() > 0) strlcpy(cfg.haToken, newToken.c_str(), sizeof(cfg.haToken));
  strlcpy(cfg.pages[0].name, newAreaName.c_str(), sizeof(cfg.pages[0].name));
  cfg.use12Hour = server.hasArg("use12h"); // unchecked checkboxes are simply absent from the POST body
  strlcpy(cfg.timezone, sanitizeTimezoneKey(server.hasArg("timezone") ? server.arg("timezone") : String(cfg.timezone)).c_str(), sizeof(cfg.timezone));
  applyTimezone();
  cfg.uiFontSize = (uint8_t)constrain(server.hasArg("uiFontSize") ? server.arg("uiFontSize").toInt() : cfg.uiFontSize, 0, 2);
  if (server.hasArg("uiTypeface")) {
    String tf = server.arg("uiTypeface");
    bool validTf = false;
    for (int i = 0; i < UI_TYPEFACES_COUNT; i++) {
      if (tf == UI_TYPEFACES[i].key) { validTf = true; break; }
    }
    if (validTf) strlcpy(cfg.uiTypeface, tf.c_str(), sizeof(cfg.uiTypeface));
  }
  cfg.uiBoldText = server.hasArg("uiBoldText");
  // Bulb color picker and web UI font picker removed from the settings
  // page for now - cfg.bulbColorKey stays at its default ("amber") and
  // cfg.webFontChoice is unused (Poppins is hardcoded in pageHeaderHtml).
  // The underlying tables/config fields are left in place in case either
  // gets a UI control again later.

  cfg.weatherLocationAuto = server.hasArg("weatherAuto");
  if (cfg.weatherLocationAuto) {
    const TzEntry& tzEntry = findTzEntry(cfg.timezone);
    cfg.weatherLat = tzEntry.lat;
    cfg.weatherLon = tzEntry.lon;
    strlcpy(cfg.weatherLocationName, tzEntry.cityLabel, sizeof(cfg.weatherLocationName));
  } else {
    if (server.hasArg("weatherLat")) cfg.weatherLat = server.arg("weatherLat").toFloat();
    if (server.hasArg("weatherLon")) cfg.weatherLon = server.arg("weatherLon").toFloat();
    if (server.hasArg("weatherName")) {
      String wn = server.arg("weatherName");
      wn.trim();
      if (wn.length() > 0) strlcpy(cfg.weatherLocationName, wn.c_str(), sizeof(cfg.weatherLocationName));
    }
  }
  // Location may have changed - drop the cached forecast so it refetches promptly.
  weatherKnown = false;
  lastWeatherFetch = 0;

  saveConfig();
  pageDirty = true;

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSaveFlash() {
  if (server.hasArg("flashRate")) {
    cfg.flashPulseRateMs = constrain(server.arg("flashRate").toInt(), 100, 5000);
  }
  if (server.hasArg("flashCount")) {
    cfg.flashPulseCount = constrain(server.arg("flashCount").toInt(), 1, 20);
  }
  if (server.hasArg("flashBrightness")) {
    cfg.flashBrightnessPct = constrain(server.arg("flashBrightness").toInt(), 1, 100);
  }

  // Checkboxes with the same name submit one value per checked box, so
  // this needs to walk every posted arg rather than server.arg(name),
  // which only returns the first match.
  String combined = "";
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) == "flashLight") {
      if (combined.length() > 0) combined += ",";
      combined += server.arg(i);
    }
  }
  strlcpy(cfg.flashLightIds, combined.c_str(), sizeof(cfg.flashLightIds));

  saveConfig();

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSaveTimers() {
  for (int i = 0; i < 5; i++) {
    String minArg = "presetMin" + String(i);
    String secArg = "presetSec" + String(i);
    int mins = server.hasArg(minArg) ? constrain(server.arg(minArg).toInt(), 0, 999) : cfg.timerPresetSec[i] / 60;
    int secs = server.hasArg(secArg) ? constrain(server.arg(secArg).toInt(), 0, 59) : cfg.timerPresetSec[i] % 60;
    int total = mins * 60 + secs;
    if (total < 1) total = 1;
    cfg.timerPresetSec[i] = total;
  }

  saveConfig();
  pageDirty = true; // Timers page may be showing the old preset labels

  server.sendHeader("Location", "/");
  server.send(303);
}

String makePageId() {
  return "area_" + String((uint32_t)millis(), HEX);
}

void handlePageAdd() {
  if (cfg.pageCount < MAX_PAGES) {
    String name = server.hasArg("name") ? server.arg("name") : "Area";
    name.trim();
    if (name.length() == 0) name = "Area";

    PageConfig& pg = cfg.pages[cfg.pageCount];
    strlcpy(pg.id, makePageId().c_str(), sizeof(pg.id));
    strlcpy(pg.name, name.c_str(), sizeof(pg.name));
    strlcpy(pg.type, "area", sizeof(pg.type));
    pg.deletable = true;
    pg.tileCount = 0;
    cfg.pageCount++;
    saveConfig();
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handlePageRename() {
  if (server.hasArg("id") && server.hasArg("name")) {
    PageConfig* pg = findPageById(server.arg("id"));
    if (pg) {
      String name = server.arg("name");
      name.trim();
      if (name.length() > 0) {
        strlcpy(pg->name, name.c_str(), sizeof(pg->name));
        saveConfig();
        pageDirty = true;
      }
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handlePageDelete() {
  if (server.hasArg("id")) {
    String id = server.arg("id");
    int foundIdx = -1;
    for (int i = 0; i < cfg.pageCount; i++) {
      if (id == cfg.pages[i].id && cfg.pages[i].deletable) { foundIdx = i; break; }
    }
    if (foundIdx >= 0) {
      for (int i = foundIdx; i < cfg.pageCount - 1; i++) cfg.pages[i] = cfg.pages[i + 1];
      cfg.pageCount--;
      if (currentPageIndex >= cfg.pageCount) currentPageIndex = cfg.pageCount - 1;
      saveConfig();
      pageDirty = true;
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleTileAdd() {
  if (server.hasArg("pageId")) {
    PageConfig* pg = findPageById(server.arg("pageId"));
    if (pg && pg->tileCount < MAX_TILES) {
      int used = 0;
      for (int i = 0; i < pg->tileCount; i++) used += pg->tiles[i].size;
      int size = server.hasArg("size") ? server.arg("size").toInt() : 1;
      if (size != 2) size = 1;

      if (used + size <= 6) {
        TileConfig& tl = pg->tiles[pg->tileCount];
        String type = server.hasArg("type") ? server.arg("type") : "light";
        String label = server.hasArg("label") ? server.arg("label") : "";
        String entityId = server.hasArg("entityId") ? server.arg("entityId") : "";
        label.trim();
        entityId.trim();
        if (label.length() == 0) label = entityId.length() > 0 ? entityId : "Tile";

        strlcpy(tl.type, type.c_str(), sizeof(tl.type));
        strlcpy(tl.label, label.c_str(), sizeof(tl.label));
        strlcpy(tl.entityId, entityId.c_str(), sizeof(tl.entityId));
        tl.size = (uint8_t)size;
        pg->tileCount++;
        saveConfig();
        pageDirty = true;
      }
    }
  }
  server.sendHeader("Location", "/page?id=" + server.arg("pageId"));
  server.send(303);
}

void handleTileDelete() {
  if (server.hasArg("pageId") && server.hasArg("index")) {
    PageConfig* pg = findPageById(server.arg("pageId"));
    int idx = server.arg("index").toInt();
    if (pg && idx >= 0 && idx < pg->tileCount) {
      for (int i = idx; i < pg->tileCount - 1; i++) pg->tiles[i] = pg->tiles[i + 1];
      pg->tileCount--;
      saveConfig();
      pageDirty = true;
    }
  }
  server.sendHeader("Location", "/page?id=" + server.arg("pageId"));
  server.send(303);
}

void handleTileMove() {
  if (server.hasArg("pageId") && server.hasArg("index") && server.hasArg("dir")) {
    PageConfig* pg = findPageById(server.arg("pageId"));
    int idx = server.arg("index").toInt();
    String dir = server.arg("dir");
    if (pg) {
      int swapWith = (dir == "up") ? idx - 1 : idx + 1;
      if (idx >= 0 && idx < pg->tileCount && swapWith >= 0 && swapWith < pg->tileCount) {
        TileConfig tmp = pg->tiles[idx];
        pg->tiles[idx] = pg->tiles[swapWith];
        pg->tiles[swapWith] = tmp;
        saveConfig();
        pageDirty = true;
      }
    }
  }
  server.sendHeader("Location", "/page?id=" + server.arg("pageId"));
  server.send(303);
}

File importUploadFile;

void handleImportUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    LittleFS.remove("/config_import.json");
    importUploadFile = LittleFS.open("/config_import.json", "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (importUploadFile) importUploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (importUploadFile) importUploadFile.close();
  }
}

void handleImportComplete() {
  bool ok = false;
  File f = LittleFS.open("/config_import.json", "r");
  if (f) {
    DynamicJsonDocument doc(12288);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    ok = !err && doc.containsKey("pages"); // basic sanity check before overwriting the live config
  }

  if (ok) {
    LittleFS.remove("/config.json");
    LittleFS.rename("/config_import.json", "/config.json");
    ok = loadConfig();
  } else {
    LittleFS.remove("/config_import.json");
  }

  String h = pageHeaderHtml("Import Config");
  if (ok) {
    h += "<h1>Import successful</h1>";
    h += "<p>Rebooting to apply the imported settings...</p>";
    h += "<meta http-equiv='refresh' content='6;url=/'>";
  } else {
    h += "<h1>Import failed</h1>";
    h += "<p>That file didn't look like a valid HA Panel config export. Nothing was changed.</p>";
    h += "<p><a href='/'>&larr; Back to settings</a></p>";
  }
  h += htmlFooter;
  server.send(200, "text/html", h);

  if (ok) {
    delay(500); // let the response flush before rebooting
    ESP.restart();
  }
}

void handleExport() {
  String json = exportConfigJson();

  String dateStr = "0.0.0";
  time_t now = time(nullptr);
  if (now > 1700000000) { // sane epoch => NTP has synced
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    char dateBuf[16];
    snprintf(dateBuf, sizeof(dateBuf), "%d.%d.%d", tmNow.tm_mon + 1, tmNow.tm_mday, tmNow.tm_year + 1900);
    dateStr = String(dateBuf);
  }

  String devicePart(cfg.deviceName);
  devicePart.toLowerCase();

  String filename = "hapanel-" + devicePart + "-" + dateStr + ".json";

  server.sendHeader("Content-Disposition", "attachment; filename=" + filename);
  server.send(200, "application/json", json);
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/page", handlePageManage);
  server.on("/save-device", HTTP_POST, handleSaveDevice);
  server.on("/save-flash", HTTP_POST, handleSaveFlash);
  server.on("/save-timers", HTTP_POST, handleSaveTimers);
  server.on("/page/add", HTTP_POST, handlePageAdd);
  server.on("/page/rename", HTTP_POST, handlePageRename);
  server.on("/page/delete", HTTP_POST, handlePageDelete);
  server.on("/tile/add", HTTP_POST, handleTileAdd);
  server.on("/tile/delete", HTTP_POST, handleTileDelete);
  server.on("/tile/move", HTTP_POST, handleTileMove);
  server.on("/tile/edit", handleTileEditForm);
  server.on("/tile/update", HTTP_POST, handleTileUpdate);
  server.on("/export", handleExport);
  server.on("/import", HTTP_POST, handleImportComplete, handleImportUpload);
  server.begin();
}

// =========================================================
// SETUP / LOOP
// =========================================================
void setup() {
  Serial.begin(115200);

  pinMode(BACKLIGHT_PIN, OUTPUT);
  analogWrite(BACKLIGHT_PIN, 255);

  tft.init();
  tft.setRotation(2);
  // tft.invertDisplay(true) was tried here but had no measurable effect -
  // your photos still show accent-colored elements (footer dot, "on" tile
  // background) as orange when blue was intended, exactly matching the
  // pre-fix symptom. Since the hardware command isn't correcting it,
  // compensating in software instead - see fixColor565() below, applied
  // to every color definition.

  touchSPI.begin(T_SCK, T_MISO, T_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(2);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }
  if (!loadConfig()) {
    setDefaultConfig();
    saveConfig();
  }
  applyTheme(cfg.darkTheme);
  applyTimezone();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected. IP address: ");
    Serial.println(WiFi.localIP());
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    // Setting TZ before configTime()'s SNTP init runs can get silently
    // overridden by that subsystem's own initialization on some ESP32
    // core versions - it doesn't reliably "stick" until re-applied after
    // sync. Wait briefly for a real epoch, then set TZ again.
    unsigned long ntpStart = millis();
    while (time(nullptr) < 1700000000 && millis() - ntpStart < 5000) {
      delay(100);
    }
    applyTimezone();

    MDNS.begin(cfg.deviceName);
    Serial.print("mDNS hostname: ");
    Serial.print(cfg.deviceName);
    Serial.println(".local");
  } else {
    Serial.println("WiFi connection failed - check WIFI_SSID/WIFI_PASSWORD in secrets.h.");
  }

  setupWebServer();

  pageDirty = true;
}

void loop() {
  server.handleClient();
  updateTimerState();
  updateFlashSequence();

  static bool dialogWasShown = false;

  if (timerExpired) {
    // Modal: skip ALL normal page drawing/polling entirely while this is
    // up. The earlier version only swapped which touch handler ran, but
    // left drawCurrentPageFull()/updateCurrentPageDynamic() running every
    // tick regardless - and updateCurrentPageDynamic() redraws individual
    // tiles whenever their content changes (the timer tile itself
    // flipping to "expired", a polled light, etc.), which painted
    // directly over the dialog since nothing was actually blocking it.
    handleTimerDialogTouch();
    ensureWeather(); // network fetch only, no screen drawing
    updateMarqueeBorder(); // edge-only, never overlaps the centered dialog

    if (!dialogWasShown) {
      drawTimerExpiredDialog();
      dialogWasShown = true;
    }
    return;
  }

  if (dialogWasShown) {
    dialogWasShown = false;
    pageDirty = true; // clean full redraw now that the dialog is gone
  }

  handleContinuousTouch();
  ensureWeather();

  if (rebootArmed && millis() - rebootArmedMs > REBOOT_CONFIRM_WINDOW_MS) {
    rebootArmed = false;
    pageDirty = true;
  }

  if (pageDirty) {
    drawCurrentPageFull();
    pollCurrentPageTiles(true);
    lastPollMs = millis();
  } else {
    updateCurrentPageDynamic();
    if (millis() - lastPollMs > POLL_INTERVAL_MS) {
      pollCurrentPageTiles(false);
      lastPollMs = millis();
    }
  }

  updateMarqueeBorder(); // re-asserted on top every tick while a timer is running
}
