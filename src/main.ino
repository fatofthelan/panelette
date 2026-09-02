// =========================================================
// Panelette
// Multi-page HA touch control panel for the CYD (ESP32-2432S028R)
//
// Implemented:
//   - Page/tile data model, stored as /config.json on LittleFS
//   - 2 col x 3 row tile grid per page (1x1 and wide 1x2 tiles)
//   - Swipe left/right between pages; footer nav shows a per-page-type
//     icon (bulb / cloud / clock / gear). Default page order is Home,
//     area pages, Forecast, Timers, Status - fully drag-reorderable in
//     the web UI (Home stays first), or reset to the default.
//   - Tile types: light (tap=toggle, hold=large brightness slider),
//     switch (tap=toggle), sensor (read-only state), scene / script /
//     button (tap fires a service, brief "Sent" flash), weather, timer,
//     sunrise / sunset / sun (Open-Meteo; "sun" is a combined 1x2 tile),
//     date / datewide (local clock; datewide = weekday + date, 1x2)
//   - Light/switch/sensor tiles: state, brightness %, and "N/A" with a
//     struck-through icon when HA reports the entity unavailable
//   - Forecast page: 5-day weather (Open-Meteo, no API key)
//   - Timers page: presets + custom time, persistent "flash lights on
//     expiry" toggle, optional flashing screen-border alert on expiry
//   - Status page: network + HA connection info (tap the HA row to
//     re-test), dark/light theme toggle, 180 screen-flip toggle, reboot
//   - Theme: colour scheme (cool/warm/phosphor/neutral) x typeface
//     (anti-aliased Noto Sans / IBM Plex Mono) x corners (rounded/square),
//     all web-UI pickers; dark/light toggled on the Status page. Every
//     primitive (cards, icons, slider) is anti-aliased.
//   - Web UI (cards: Device / Home Assistant / Weather / Network / Pages /
//     Timers / Backup): device + theme settings, HA URL/token, per-page
//     hide toggle, add/remove pages and tiles, drag-to-reorder,
//     toggle-style checkboxes, segmented Back up / Restore
//   - Network card: DHCP or static IP (IP/subnet/gateway + optional DNS),
//     validated, reboots to apply, DHCP fallback if a static IP won't
//     associate. Web UI also has a Reboot button and an unsaved-edits guard.
//   - HA setup helpers (all HTTP-only, each falls back to manual entry):
//       * mDNS discovery of the HA URL on boot
//       * /api/config import (time zone, location) on Save
//       * connection test - "Test" button in the web UI, HA row on the
//         Status page
//       * entity picker (datalist) in the tile forms, via /api/template
//       * "Add from a Home Assistant area" - group-aware checklist
//   - Optional HA WebSocket live updates (cfg.haLiveUpdates, off by
//     default): ws:// only, subscribe_entities on the tracked entity set,
//     REST poll stays as a slow backstop. See the haWs* module.
//
// Deferred:
//   - No-build-tools install path (so a non-technical HA user needs no
//     IDE, no libraries, no User_Setup.h):
//       * captive-portal WiFi onboarding (panel boots as its own AP,
//         you join it and pick your network) - also removes secrets.h
//       * GitHub Releases with merged firmware.bin (bootloader + parts +
//         app), built on the min_spiffs partition
//       * an ESP Web Tools page (GitHub Pages) - flash from Chrome/Edge
//         with one click, no install
// =========================================================

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h> // captive-portal DNS during Wi-Fi provisioning
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Preferences.h> // NVS-backed store for Wi-Fi credentials (see the wifiCreds* module)
#include <XPT2046_Touchscreen.h>
#include <WebSocketsClient.h> // optional HA WebSocket live updates - see haWs* module
#include <TFT_eSPI.h>
// Do NOT #include any Fonts/GFXFF/*.h here. The UI uses anti-aliased .vlw
// fonts now (see the *.h includes below), but LOAD_GFXFF is still set in
// build_flags, so TFT_eSPI.h already pulls in every GFXFF header - and
// they have no include guards, so re-including caused redefinition errors.
#include <time.h>
#include "config_types.h"
// secrets.h is now OPTIONAL. A source build can still drop real Wi-Fi
// credentials in include/secrets.h (gitignored, copy from
// secrets.h.example) and they take priority; without it, the panel gets
// online via stored credentials or the on-device setup flow. Browser /
// CI builds compile fine with no secrets.h at all.
#if __has_include("secrets.h")
  #include "secrets.h" // #defines WIFI_SSID / WIFI_PASSWORD
#endif
#include "NotoSansB18.h" // anti-aliased .vlw fonts: Noto Sans (typeface "sans")
#include "NotoSansB30.h"
#include "PlexMono16.h"  //                          IBM Plex Mono (typeface "mono")
#include "PlexMono26.h"

// =========================================================
// WIFI / HA DEFAULTS
// =========================================================
// WiFi credentials live in include/secrets.h. HA URL/token can also be set
// later from the web UI and are stored in /config.json from then on.
const char* HA_URL_DEFAULT = "http://homeassistant.local:8123";

// Firmware identity. FW_VERSION is shown on the Status page, in the web UI
// header, and reported over Improv-Serial so the browser installer can
// tell "install" from "update".
#define FW_NAME    "Panelette"
#define FW_VERSION "1.0.0"

// Timers-page preset buttons, in seconds: 1 / 5 / 10 / 15 / 30 min.
const int DEFAULT_TIMER_PRESETS_SEC[5] = {60, 300, 600, 900, 1800};

// Default device / hostname: "Panelette" + the last 4 hex digits of the MAC,
// so two fresh panels on the same network don't collide. Falls back to a
// random 4-hex suffix if the efuse MAC reads back as zero.
void makeDefaultDeviceName(char* out, size_t n) {
  uint16_t suffix = (uint16_t)((ESP.getEfuseMac() >> 32) & 0xFFFF);
  if (suffix == 0) suffix = (uint16_t)(esp_random() & 0xFFFF);
  snprintf(out, n, "Panelette%04X", suffix);
}

// Keep only characters valid in a hostname / mDNS name; spaces and
// underscores become hyphens, leading/trailing hyphens trimmed. Empty
// result falls back to a generated default.
String sanitizeHostname(const String& in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (isalnum((unsigned char)c) || c == '-') out += c;
    else if (c == ' ' || c == '_') out += '-';
  }
  while (out.length() && out[0] == '-') out.remove(0, 1);
  while (out.length() && out[out.length() - 1] == '-') out.remove(out.length() - 1);
  if (out.length() == 0) {
    char buf[24];
    makeDefaultDeviceName(buf, sizeof(buf));
    out = buf;
  }
  return out;
}

static uint32_t ipToU32(const IPAddress& a) {
  return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3];
}

// Validates a static-IP triple. Returns "" if OK, else a message for the UI.
// (No cfg access - safe to keep near the other string helpers.)
String validateStaticNet(const String& ip, const String& sn, const String& gw) {
  IPAddress a, m, g;
  if (!a.fromString(ip) || ipToU32(a) == 0) return "IP address is not valid.";
  if (!m.fromString(sn)) return "Subnet mask is not valid.";
  if (!g.fromString(gw) || ipToU32(g) == 0) return "Gateway is not valid.";

  uint32_t mask = ipToU32(m);
  uint32_t inv = ~mask;
  if (mask == 0 || (inv & (inv + 1)) != 0) return "Subnet mask must be contiguous (e.g. 255.255.255.0).";
  if ((ipToU32(a) & mask) != (ipToU32(g) & mask)) return "IP address and gateway are on different subnets.";
  if (ipToU32(a) == ipToU32(g)) return "IP address and gateway can't be the same.";
  return "";
}

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
// gCardRadius (rounded/square) is a runtime value - see applyCornerStyle()

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

// Colour scheme axis. Each has a dark + light variant; the 7 values are
// BG / PANEL / PANEL_ALT / STROKE / TEXT / DIM / ACCENT (RGB565, pre-inversion).
struct Palette { uint16_t bg, panel, alt, stroke, text, dim, accent; };
struct ColorScheme { const char* key; const char* label; Palette dark, light; };
const ColorScheme COLOR_SCHEMES[] = {
  {"cool", "Cool (teal)",
   {0x10A3,0x1905,0x2167,0x31C8,0xEF7E,0x8C94,0x2DF5}, {0xEF5D,0xFFFF,0xEF9E,0xD6DB,0x1926,0x638F,0x0CF1}},
  {"warm", "Warm (amber)",
   {0x10A1,0x39A5,0x4A27,0x6B0A,0xF79C,0xAD11,0xFCE5}, {0xE71A,0xFFDE,0xEF5B,0xD677,0x2924,0x83EE,0xB343}},
  {"phosphor", "Phosphor (green / amber CRT)",
   {0x0861,0x10A2,0x1903,0x29A6,0xD6FA,0x638D,0x5794}, {0x1060,0x1880,0x28E1,0x3962,0xF651,0x8B47,0xFDA9}},
  {"neutral", "Neutral (grey)",
   {0x10C3,0x2125,0x2987,0x39E8,0xE75D,0x8C93,0x6C77}, {0xEF7E,0xFFFF,0xF7BE,0xDF1C,0x18E4,0x634E,0x3B32}},
};
const int COLOR_SCHEMES_COUNT = sizeof(COLOR_SCHEMES) / sizeof(COLOR_SCHEMES[0]);

bool showTileBorder;
void applyTheme(bool dark); // defined after DeviceConfig cfg (it reads cfg)

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

// Set true this session if a configured static IP failed to associate and
// we fell back to DHCP (shown on the Status page and in the web UI).
bool staticIpFellBack = false;

void applyTheme(bool dark) {
  const ColorScheme* cs = &COLOR_SCHEMES[0];
  for (int i = 0; i < COLOR_SCHEMES_COUNT; i++)
    if (strcmp(cfg.colorScheme, COLOR_SCHEMES[i].key) == 0) { cs = &COLOR_SCHEMES[i]; break; }
  const Palette& p = dark ? cs->dark : cs->light;

  COL_BG        = fixColor565(p.bg);
  COL_PANEL     = fixColor565(p.panel);
  COL_PANEL_ALT = fixColor565(p.alt);
  COL_STROKE    = fixColor565(p.stroke);
  COL_TEXT      = fixColor565(p.text);
  COL_DIM       = fixColor565(p.dim);
  COL_ACCENT    = fixColor565(p.accent);

  // Rounded + light = borderless cards with a drop shadow; otherwise a
  // hairline border on every card.
  bool rounded = (strcmp(cfg.cornerStyle, "square") != 0);
  showTileBorder = !(rounded && !dark);
  COL_PANEL_LIT = blendColor565(COL_PANEL, COL_ACCENT, dark ? 20 : 12);
}

// Portrait either way; rotation 2 = USB on one side, 0 = flipped 180.
// Touch stays on rotation 2 (its calibration was measured there) and
// readTouchXY() point-reflects the mapped coords when flipped instead.
void applyScreenRotation() {
  tft.setRotation(cfg.flipScreen ? 0 : 2);
}

// Applied once at boot, before WiFi.begin(). No-op for DHCP or if the
// stored values don't parse (setup() then also has a DHCP fallback).
void applyNetworkConfig() {
  if (!cfg.useStaticIp) return;
  IPAddress ip, sn, gw, d1, d2;
  if (!ip.fromString(cfg.ipAddr) || !sn.fromString(cfg.subnet) || !gw.fromString(cfg.gateway)) return;
  if (strlen(cfg.dns1) == 0 || !d1.fromString(cfg.dns1)) d1 = gw;
  if (strlen(cfg.dns2) == 0 || !d2.fromString(cfg.dns2)) d2 = IPAddress((uint32_t)0);
  WiFi.config(ip, gw, sn, d1, d2);
}

// =========================================================
// WI-FI CREDENTIALS
// =========================================================
// Credentials live in NVS (the ESP32 key-value flash store), NOT in
// /config.json - so a config reset or a LittleFS reformat can't lock the
// panel off the network, and they're never part of an exported backup.
// A real include/secrets.h (source builds) overrides them.
//
// Resolution order, boot: compile-time secrets.h  ->  NVS  ->  none
// (none = the on-device setup flow takes over, added in a later step).

Preferences wifiPrefs;
const char* WIFI_NVS_NAMESPACE = "wifi";

char gWifiSsid[33] = "";  // 32-char max SSID + NUL
char gWifiPass[65] = "";  // 63-char max passphrase (or 64-hex PSK) + NUL
WifiCredSource gWifiCredSource = WCS_NONE; // enum in config_types.h

// true if a genuine (non-placeholder) SSID was baked in via secrets.h.
bool compileWifiUsable() {
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
  const char* s = WIFI_SSID;
  return strlen(s) > 0 && strcmp(s, "your-wifi-ssid") != 0;
#else
  return false;
#endif
}

bool wifiCredsLoadFromNvs(char* ssid, size_t ssidN, char* pass, size_t passN) {
  // read-write open so a first-ever read creates the namespace instead of
  // logging nvs_open NOT_FOUND; getString() itself writes nothing.
  wifiPrefs.begin(WIFI_NVS_NAMESPACE, /*readOnly=*/false);
  String s = wifiPrefs.getString("ssid", "");
  String p = wifiPrefs.getString("pass", "");
  wifiPrefs.end();
  if (s.length() == 0) return false;
  strlcpy(ssid, s.c_str(), ssidN);
  strlcpy(pass, p.c_str(), passN);
  return true;
}

void wifiCredsSaveToNvs(const char* ssid, const char* pass) {
  wifiPrefs.begin(WIFI_NVS_NAMESPACE, /*readOnly=*/false);
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("pass", pass);
  wifiPrefs.end();
}

void wifiCredsClearNvs() {
  wifiPrefs.begin(WIFI_NVS_NAMESPACE, /*readOnly=*/false);
  wifiPrefs.clear();
  wifiPrefs.end();
}

// Picks the credentials to use and records where they came from. Also
// mirrors a secrets.h SSID into NVS the first time it's seen, so a later
// no-secrets build flashed onto the same chip (without a full erase) still
// comes up on Wi-Fi.
WifiCredSource wifiResolveCreds() {
  if (compileWifiUsable()) {
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    strlcpy(gWifiSsid, WIFI_SSID, sizeof(gWifiSsid));
    strlcpy(gWifiPass, WIFI_PASSWORD, sizeof(gWifiPass));
#endif
    char ns[33] = "", np[65] = "";
    if (!wifiCredsLoadFromNvs(ns, sizeof(ns), np, sizeof(np)) ||
        strcmp(ns, gWifiSsid) != 0 || strcmp(np, gWifiPass) != 0) {
      wifiCredsSaveToNvs(gWifiSsid, gWifiPass);
    }
    gWifiCredSource = WCS_COMPILE;
    return gWifiCredSource;
  }
  if (wifiCredsLoadFromNvs(gWifiSsid, sizeof(gWifiSsid), gWifiPass, sizeof(gWifiPass))) {
    gWifiCredSource = WCS_STORED;
    return gWifiCredSource;
  }
  gWifiSsid[0] = gWifiPass[0] = '\0';
  gWifiCredSource = WCS_NONE;
  return gWifiCredSource;
}

// =========================================================
// WI-FI PROVISIONING  (on-device setup when there are no working creds)
// =========================================================
// Entered from setup() when wifiResolveCreds() came up empty or the
// resolved credentials wouldn't connect. The panel runs as its own open
// Wi-Fi AP ("PaneletteXXXX") with a captive portal; the user picks their
// network and enters the password, we save it to NVS and reboot. If
// credentials *do* exist (they just failed at boot - router was down,
// etc.) we also keep retrying them in the background.
//
// The setup page is deliberately tiny - Wi-Fi only. The full config UI is
// not served until the panel is actually online.

DNSServer dnsServer;
bool gProvisioning = false;
char gApSsid[24] = "";
String gProvScanHtml = "<option>(scanning...)</option>"; // <option> list for the SSID picker
unsigned long gProvRetryMs = 0;
bool gProvScreenDrawn = false;

// Rebuild the cached SSID <option> list. Blocking (~2-4 s); only called
// from the provisioning path where nothing else is time-critical.
void provScanNetworks() {
  int n = WiFi.scanNetworks(false, false);
  String html;
  for (int i = 0; i < n && i < 20; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    if (html.indexOf(">" + s + "<") >= 0) continue; // dedupe (mesh)
    html += "<option>" + s + "</option>";
  }
  WiFi.scanDelete();
  gProvScanHtml = html.length() ? html : String("<option>(no networks found)</option>");
}

void startProvisioning() {
  gProvisioning = true;
  gProvScreenDrawn = false;
  if (gApSsid[0] == '\0') makeDefaultDeviceName(gApSsid, sizeof(gApSsid)); // "PaneletteXXXX"

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(gApSsid); // open network - conventional for setup APs
  delay(150);
  IPAddress apIp = WiFi.softAPIP();               // 192.168.4.1
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", apIp);                 // every lookup -> the panel

  provScanNetworks();
  Serial.printf("[prov] setup AP '%s' at http://%s/\n", gApSsid, apIp.toString().c_str());
}

// =========================================================
// IMPROV-SERIAL  (hand-rolled - improv-wifi.com/serial)
// =========================================================
// Lets the browser installer (ESP Web Tools) read the firmware
// name/version (to offer "update" vs "install") and hand over Wi-Fi
// credentials right after flashing, over the same USB serial line. Runs in
// loop() at all times; ~no cost when the host isn't talking.
//
// Packet: 'IMPROV' | ver(1) | type(1) | len(1) | data(len) | checksum(1)
//   checksum = sum(all preceding bytes) & 0xFF
// type: 1=CurrentState 2=ErrorState 3=RPC(host->dev) 4=RPCResult(dev->host)

enum { IMP_TYPE_STATE = 1, IMP_TYPE_ERROR = 2, IMP_TYPE_RPC = 3, IMP_TYPE_RPC_RESULT = 4 };
enum { IMP_STATE_READY = 2, IMP_STATE_PROVISIONING = 3, IMP_STATE_PROVISIONED = 4 };
enum { IMP_ERR_NONE = 0, IMP_ERR_BAD_PACKET = 1, IMP_ERR_UNKNOWN_CMD = 2, IMP_ERR_CANT_CONNECT = 3 };
enum { IMP_CMD_WIFI = 1, IMP_CMD_IDENTIFY = 2, IMP_CMD_GET_STATE = 3, IMP_CMD_GET_INFO = 4, IMP_CMD_GET_NETWORKS = 5 };

uint8_t improvBuf[300];
size_t  improvLen = 0;
bool    improvHostSeen = false;      // a host has sent us a valid packet
unsigned long improvAnnounceMs = 0;  // last unprompted CURRENT_STATE

void improvSend(uint8_t type, const uint8_t* data, uint8_t len) {
  uint8_t pkt[9 + 256 + 1];
  memcpy(pkt, "IMPROV", 6);
  pkt[6] = 1; pkt[7] = type; pkt[8] = len;
  if (len) memcpy(pkt + 9, data, len);
  uint32_t sum = 0;
  for (int i = 0; i < 9 + len; i++) sum += pkt[i];
  pkt[9 + len] = sum & 0xFF;
  Serial.write(pkt, 9 + len + 1);
  Serial.flush();
}

void improvSendState(uint8_t s)  { uint8_t b = s; improvSend(IMP_TYPE_STATE, &b, 1); }
void improvSendError(uint8_t e)  { uint8_t b = e; improvSend(IMP_TYPE_ERROR, &b, 1); }

// RPC result: cmdId | resultLen | (len-prefixed strings). `strs` already
// packed as [len][bytes] repeated in `packed`.
void improvSendResult(uint8_t cmdId, const uint8_t* packed, uint8_t packedLen) {
  uint8_t d[2 + 256];
  d[0] = cmdId; d[1] = packedLen;
  if (packedLen) memcpy(d + 2, packed, packedLen);
  improvSend(IMP_TYPE_RPC_RESULT, d, 2 + packedLen);
}

uint8_t improvPackStr(uint8_t* out, uint8_t pos, const char* s) {
  uint8_t l = strlen(s);
  out[pos++] = l;
  memcpy(out + pos, s, l);
  return pos + l;
}

uint8_t improvCurrentState() {
  return (WiFi.status() == WL_CONNECTED) ? IMP_STATE_PROVISIONED : IMP_STATE_READY;
}

void improvSendDeviceUrl(uint8_t cmdId) {
  String url = String("http://") + WiFi.localIP().toString();
  uint8_t p[128];
  uint8_t n = improvPackStr(p, 0, url.c_str());
  improvSendResult(cmdId, p, n);
}

void improvHandleWifi(const uint8_t* cd, uint8_t cdLen) {
  if (cdLen < 2) { improvSendError(IMP_ERR_BAD_PACKET); return; }
  uint8_t sl = cd[0];
  if ((size_t)1 + sl + 1 > cdLen) { improvSendError(IMP_ERR_BAD_PACKET); return; }
  uint8_t pl = cd[1 + sl];
  if ((size_t)2 + sl + pl > cdLen || sl > 32 || pl > 64) { improvSendError(IMP_ERR_BAD_PACKET); return; }

  char ssid[33], pass[65];
  memcpy(ssid, cd + 1, sl);        ssid[sl] = '\0';
  memcpy(pass, cd + 2 + sl, pl);   pass[pl] = '\0';
  Serial.printf("[improv] Wi-Fi settings for '%s'\n", ssid);

  improvSendState(IMP_STATE_PROVISIONING);

  // Connect FIRST, save only on success. WiFi.status() can read stale
  // WL_CONNECTED while switching networks, so also require a real IP.
  WiFi.disconnect(false, true);
  delay(200);
  WiFi.begin(ssid, pass);
  unsigned long t = millis();
  while (millis() - t < 15000) {
    wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED && WiFi.localIP() != IPAddress((uint32_t)0)) break;
    if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL) break;
    delay(200);
  }
  bool ok = (WiFi.status() == WL_CONNECTED) && (WiFi.localIP() != IPAddress((uint32_t)0));

  if (ok) {
    wifiCredsSaveToNvs(ssid, pass);
    improvSendState(IMP_STATE_PROVISIONED);
    improvSendDeviceUrl(IMP_CMD_WIFI);
    Serial.printf("[improv] connected as %s - restarting\n", WiFi.localIP().toString().c_str());
    delay(500);
    ESP.restart();
  } else {
    improvSendError(IMP_ERR_CANT_CONNECT);
    Serial.println("[improv] could not connect - reverting");
    WiFi.disconnect(false, true);
    delay(200);
    if (gWifiSsid[0]) WiFi.begin(gWifiSsid, gWifiPass); // back onto the booted network
  }
}

void improvHandleGetNetworks() {
  int n = WiFi.scanNetworks(false, false);
  for (int i = 0; i < n && i < 20; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    char rssi[8]; snprintf(rssi, sizeof(rssi), "%d", WiFi.RSSI(i));
    bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    uint8_t p[160]; uint8_t pos = 0;
    pos = improvPackStr(p, pos, ssid.c_str());
    pos = improvPackStr(p, pos, rssi);
    pos = improvPackStr(p, pos, open ? "NO" : "YES");
    improvSendResult(IMP_CMD_GET_NETWORKS, p, pos);
  }
  WiFi.scanDelete();
  improvSendResult(IMP_CMD_GET_NETWORKS, nullptr, 0); // end marker
}

void improvHandleRpc(const uint8_t* data, uint8_t dataLen) {
  if (dataLen < 2) { improvSendError(IMP_ERR_BAD_PACKET); return; }
  uint8_t cmd = data[0];
  uint8_t cdLen = data[1];
  if ((size_t)2 + cdLen > dataLen) { improvSendError(IMP_ERR_BAD_PACKET); return; }
  const uint8_t* cd = data + 2;

  switch (cmd) {
    case IMP_CMD_GET_STATE:
      improvSendState(improvCurrentState());
      if (WiFi.status() == WL_CONNECTED) improvSendDeviceUrl(IMP_CMD_GET_STATE);
      break;
    case IMP_CMD_GET_INFO: {
      uint8_t p[160]; uint8_t pos = 0;
      pos = improvPackStr(p, pos, FW_NAME);
      pos = improvPackStr(p, pos, FW_VERSION);
      pos = improvPackStr(p, pos, "ESP32");
      pos = improvPackStr(p, pos, cfg.deviceName);
      improvSendResult(IMP_CMD_GET_INFO, p, pos);
      break;
    }
    case IMP_CMD_GET_NETWORKS:
      improvHandleGetNetworks();
      break;
    case IMP_CMD_WIFI:
      improvHandleWifi(cd, cdLen);
      break;
    case IMP_CMD_IDENTIFY:
      tft.fillScreen(COL_ACCENT);
      delay(600);
      if (gProvisioning) gProvScreenDrawn = false; // handleProvisioning redraws
      else               drawCurrentPageFull();
      break;
    default:
      improvSendError(IMP_ERR_UNKNOWN_CMD);
      break;
  }
}

void improvHandlePacket(const uint8_t* pkt, size_t total) {
  if (pkt[6] != 1) return;
  uint32_t sum = 0;
  for (size_t i = 0; i < total - 1; i++) sum += pkt[i];
  if ((sum & 0xFF) != pkt[total - 1]) { improvSendError(IMP_ERR_BAD_PACKET); return; }

  improvHostSeen = true; // a real host is talking - stop the boot announce
  uint8_t type = pkt[7];
  uint8_t len  = pkt[8];
  if (type == IMP_TYPE_RPC) improvHandleRpc(pkt + 9, len);
  // other inbound types (state/error/result) are host->device only for RPC;
  // nothing to do.
}

// Called every loop(). Scans the serial stream for IMPROV packets, and for
// the first stretch after boot announces CURRENT_STATE unprompted (~1.5 s) -
// ESP Web Tools sends a single GET_CURRENT_STATE with a ~1 s timeout right
// after opening the port, which resets the ESP32, so that probe is usually
// lost. Any CURRENT_STATE packet resolves the installer's wait.
void improvLoop() {
  if (!improvHostSeen && millis() < 20000 && millis() - improvAnnounceMs >= 1500) {
    improvAnnounceMs = millis();
    improvSendState(improvCurrentState());
  }

  while (Serial.available()) {
    uint8_t b = Serial.read();

    if (improvLen < 6) { // syncing on the "IMPROV" header
      const char* H = "IMPROV";
      if (b == (uint8_t)H[improvLen]) {
        improvBuf[improvLen++] = b;
      } else {
        improvLen = (b == 'I') ? 1 : 0;
        if (improvLen) improvBuf[0] = 'I';
      }
      continue;
    }

    improvBuf[improvLen++] = b;
    if (improvLen >= 9) {
      size_t total = 9 + (size_t)improvBuf[8] + 1; // header+data+checksum
      if (improvLen >= total) {
        improvHandlePacket(improvBuf, total);
        improvLen = 0;
      }
    }
    if (improvLen >= sizeof(improvBuf)) improvLen = 0; // runaway guard
  }
}

// delay() that keeps answering Improv - used in setup()'s blocking waits so
// the browser installer can detect the panel even while it's still booting.
void improvDelay(unsigned long ms) {
  unsigned long t = millis();
  while (millis() - t < ms) { improvLoop(); delay(10); }
}

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
// THEME AXES: typeface (sans / mono) + corner style. Colour scheme is
// handled in applyTheme(). All three are pickable in the web UI.
// =========================================================
// The typeface is always an anti-aliased .vlw font. "sans" = Noto Sans
// (Title Case, rounded feel); "mono" = IBM Plex Mono, which also turns on
// UPPERCASE + hairline label rules for the "instrument" look.
bool gSmoothFont = false;        // an AA font is loaded (always true after applyTypeface)
bool gMono       = false;        // typeface == "mono"
bool gForceUpper = false;        // uppercase every drawn string (mono only)
bool gTileRule   = false;        // hairline rule under tile labels (mono only)
int  gCardRadius = 14;           // rounded-rect corner: 14 rounded / 3 square
const uint8_t* gFontBase = NotoSansB18_vlw;
const uint8_t* gFontBig  = NotoSansB30_vlw;

void applyTypeface() {
  gMono = (strcmp(cfg.uiTypeface, "mono") == 0);
  gForceUpper = gMono;
  gTileRule   = gMono;

  const uint8_t* base = gMono ? PlexMono16_vlw : NotoSansB18_vlw;
  gFontBig            = gMono ? PlexMono26_vlw : NotoSansB30_vlw;

  if (!gSmoothFont || base != gFontBase) {
    if (gSmoothFont) { tft.unloadFont(); sprTile.unloadFont(); }
    tft.loadFont(base);
    sprTile.loadFont(base);
    gSmoothFont = true;
    gFontBase = base;
  }
}

void applyCornerStyle() {
  gCardRadius = (strcmp(cfg.cornerStyle, "square") == 0) ? 3 : 14;
}

// =========================================================
// Anti-aliased primitive wrappers - "smooth all the things".
// =========================================================
// Every rounded rect / circle on the device goes through these instead of
// the jagged Bresenham fillRoundRect/drawRoundRect/fillCircle/drawCircle.
// `bg` is the colour the anti-aliased edge blends toward: pass the surface
// the shape sits on (COL_BG for anything drawn straight onto the screen,
// the local panel/sprite fill otherwise). UI_AA_READ samples the actual
// pixel underneath - correct for shapes drawn into a sprite whose local
// background varies, at the cost of a per-edge-pixel read.
static const uint32_t UI_AA_READ = 0x00FFFFFF;

template<typename T>
void uiFillRR(T& d, int x, int y, int w, int h, int r, uint16_t col, uint32_t bg = COL_BG) {
  d.fillSmoothRoundRect(x, y, w, h, r, col, bg);
}

template<typename T>
void uiStrokeRR(T& d, int x, int y, int w, int h, int r, uint16_t col, uint32_t bg = COL_BG) {
  d.drawSmoothRoundRect(x, y, r, r > 1 ? r - 1 : 0, w, h, col, bg);
}

template<typename T>
void uiFillCircle(T& d, int x, int y, int r, uint16_t col, uint32_t bg = UI_AA_READ) {
  d.fillSmoothCircle(x, y, r, col, bg);
}

// Anti-aliased ring: outer disc in `col`, inner disc (r - thick) punched
// back to `inner`. Both edges sample the real background so it composites
// cleanly over sprites and panels alike.
template<typename T>
void uiRingCircle(T& d, int x, int y, int r, uint16_t col, uint16_t inner, int thick = 2) {
  d.fillSmoothCircle(x, y, r, col, UI_AA_READ);
  if (r - thick > 0) d.fillSmoothCircle(x, y, r - thick, inner, UI_AA_READ);
}

// Every on-device text draw goes through these. The active .vlw font is
// always loaded (gFontBase); fontNum >= 4 swaps in the larger cut for the
// duration of the call. gForceUpper (mono typeface) uppercases everything.
template<typename T>
void uiDrawString(T& d, const String& textIn, int x, int y, int fontNum) {
  String text = textIn;
  if (gForceUpper) text.toUpperCase();
  bool big = (fontNum >= 4);
  if (big) d.loadFont(gFontBig);
  d.drawString(text, x, y);
  if (big) d.loadFont(gFontBase);
}

template<typename T>
int uiTextWidth(T& d, const String& text, int fontNum) {
  bool big = (fontNum >= 4);
  if (big) d.loadFont(gFontBig);
  int w = d.textWidth(text);
  if (big) d.loadFont(gFontBase);
  return w;
}

// Shortens text (dropping characters from the end, adding "..." unless
// ellipsis=false) until it fits within maxWidth, then draws it. The .vlw
// fonts only have the base + big cuts, no small tile-scale tier, so this
// truncates rather than shrinking. Draws directly (not return-then-draw)
// since which cut is loaded matters.
template<typename T>
void uiDrawFitted(T& d, const String& text, int x, int y, int maxWidth, int preferredFont, bool ellipsis = true) {
  int useFont = preferredFont;

  String fitted = text;
  if (uiTextWidth(d, fitted, useFont) > maxWidth) {
    const String tail = ellipsis ? String("...") : String("");
    int tailW = uiTextWidth(d, tail, useFont);
    while (fitted.length() > 1 && uiTextWidth(d, fitted, useFont) + tailW > maxWidth) {
      fitted.remove(fitted.length() - 1);
    }
    fitted += tail;
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
  makeDefaultDeviceName(cfg.deviceName, sizeof(cfg.deviceName));
  strlcpy(cfg.haUrl, HA_URL_DEFAULT, sizeof(cfg.haUrl));
  cfg.haToken[0] = '\0';
  cfg.haLiveUpdates = false;
  cfg.darkTheme = true;
  cfg.use12Hour = false;
  cfg.flipScreen = false;
  cfg.uiFontSize = 1;
  strlcpy(cfg.uiTypeface, "mono", sizeof(cfg.uiTypeface));           // sans | mono
  strlcpy(cfg.colorScheme, "phosphor", sizeof(cfg.colorScheme));     // cool | warm | phosphor | neutral
  strlcpy(cfg.cornerStyle, "square", sizeof(cfg.cornerStyle));       // rounded | square
  cfg.uiBoldText = false;
  strlcpy(cfg.bulbColorKey, "amber", sizeof(cfg.bulbColorKey));
  strlcpy(cfg.timezone, "us_pacific", sizeof(cfg.timezone));

  const TzEntry& defaultTz = findTzEntry(cfg.timezone);
  cfg.weatherLat = defaultTz.lat;
  cfg.weatherLon = defaultTz.lon;
  strlcpy(cfg.weatherLocationName, defaultTz.cityLabel, sizeof(cfg.weatherLocationName));
  strlcpy(cfg.webFontChoice, "inter", sizeof(cfg.webFontChoice));

  cfg.flashLightIds[0] = '\0';
  cfg.flashPulseRateMs = 500;
  cfg.flashPulseCount = 5;
  cfg.flashBrightnessPct = 25;
  cfg.flashOnExpire = true;
  cfg.marqueeEnabled = true;

  for (int i = 0; i < 5; i++) cfg.timerPresetSec[i] = DEFAULT_TIMER_PRESETS_SEC[i];

  cfg.customPageOrder = false;

  cfg.useStaticIp = false;
  cfg.ipAddr[0] = cfg.subnet[0] = cfg.gateway[0] = cfg.dns1[0] = cfg.dns2[0] = '\0';

  cfg.pageCount = 4;

  const char* defIds[4]   = {"home", "forecast", "timers", "status"};
  const char* defNames[4] = {"Home", "Forecast", "Timers", "Status"};
  for (int i = 0; i < 4; i++) {
    strlcpy(cfg.pages[i].id, defIds[i], sizeof(cfg.pages[i].id));
    strlcpy(cfg.pages[i].name, defNames[i], sizeof(cfg.pages[i].name));
    strlcpy(cfg.pages[i].type, defIds[i], sizeof(cfg.pages[i].type)); // id == type for the built-ins
    cfg.pages[i].deletable = false;
    cfg.pages[i].hidden = false;
    cfg.pages[i].tileCount = 0;
  }
}

// Whether a page can be hidden from the on-device footer/swipe nav. Home
// is the fixed first page; Status is the only on-device route to reboot /
// theme / HA test, so neither can be hidden.
bool pageCanHide(const char* type) {
  return strcmp(type, "home") != 0 && strcmp(type, "status") != 0;
}

// Footer / swipe order: Home first, then area (control) pages in the order
// they were added, then Forecast, Timers, and Status last. Everything -
// footer icons, page dots, swipe nav - follows cfg.pages[] order, so this
// is enforced in the data model rather than per-view. Home keeps index 0
// (several call sites treat cfg.pages[0] as the home page).
int pageSortKey(const char* type) {
  if (strcmp(type, "home") == 0) return 0;
  if (strcmp(type, "forecast") == 0) return 2;
  if (strcmp(type, "timers") == 0) return 3;
  if (strcmp(type, "status") == 0) return 4;
  return 1; // "area" and any future control-page type
}

void sortPages() {
  // stable insertion sort (pageCount is <= MAX_PAGES = 8)
  for (int i = 1; i < cfg.pageCount; i++) {
    PageConfig tmp = cfg.pages[i];
    int key = pageSortKey(tmp.type);
    int j = i - 1;
    while (j >= 0 && pageSortKey(cfg.pages[j].type) > key) {
      cfg.pages[j + 1] = cfg.pages[j];
      j--;
    }
    cfg.pages[j + 1] = tmp;
  }
}

// --- Visible-page navigation (hidden pages are skipped on the device) ---
int visiblePageCount() {
  int n = 0;
  for (int i = 0; i < cfg.pageCount; i++) if (!cfg.pages[i].hidden) n++;
  return n;
}

// 0-based position of a page among the visible ones, or -1 if it's hidden.
int visiblePosOf(int arrIdx) {
  int p = 0;
  for (int i = 0; i < cfg.pageCount; i++) {
    if (i == arrIdx) return cfg.pages[i].hidden ? -1 : p;
    if (!cfg.pages[i].hidden) p++;
  }
  return -1;
}

int nextVisiblePage(int arrIdx) {
  for (int i = arrIdx + 1; i < cfg.pageCount; i++) if (!cfg.pages[i].hidden) return i;
  return arrIdx;
}

int prevVisiblePage(int arrIdx) {
  for (int i = arrIdx - 1; i >= 0; i--) if (!cfg.pages[i].hidden) return i;
  return arrIdx;
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

  strlcpy(cfg.deviceName, doc["deviceName"] | "", sizeof(cfg.deviceName));
  if (strlen(cfg.deviceName) == 0) makeDefaultDeviceName(cfg.deviceName, sizeof(cfg.deviceName));
  strlcpy(cfg.haUrl, doc["haUrl"] | HA_URL_DEFAULT, sizeof(cfg.haUrl));
  strlcpy(cfg.haToken, doc["haToken"] | "", sizeof(cfg.haToken));
  cfg.haLiveUpdates = doc["haLiveUpdates"] | false;
  cfg.darkTheme = doc["darkTheme"] | true;
  cfg.use12Hour = doc["use12Hour"] | false;
  cfg.flipScreen = doc["flipScreen"] | false;
  cfg.uiFontSize = (uint8_t)constrain((int)(doc["uiFontSize"] | 1), 0, 2);
  strlcpy(cfg.uiTypeface, doc["uiTypeface"] | "sans", sizeof(cfg.uiTypeface));
  if (strcmp(cfg.uiTypeface, "sans") != 0 && strcmp(cfg.uiTypeface, "mono") != 0)
    strlcpy(cfg.uiTypeface, "sans", sizeof(cfg.uiTypeface)); // migrate old values
  strlcpy(cfg.colorScheme, doc["colorScheme"] | "cool", sizeof(cfg.colorScheme));
  strlcpy(cfg.cornerStyle, doc["cornerStyle"] | "rounded", sizeof(cfg.cornerStyle));
  cfg.uiBoldText = doc["uiBoldText"] | false;
  strlcpy(cfg.bulbColorKey, doc["bulbColorKey"] | "amber", sizeof(cfg.bulbColorKey));
  strlcpy(cfg.timezone, doc["timezone"] | "us_pacific", sizeof(cfg.timezone));
  strlcpy(cfg.timezone, sanitizeTimezoneKey(cfg.timezone).c_str(), sizeof(cfg.timezone));

  const TzEntry& tzForDefault = findTzEntry(cfg.timezone);
  cfg.weatherLat = doc["weatherLat"] | tzForDefault.lat;
  cfg.weatherLon = doc["weatherLon"] | tzForDefault.lon;
  strlcpy(cfg.weatherLocationName, doc["weatherLocationName"] | tzForDefault.cityLabel, sizeof(cfg.weatherLocationName));
  strlcpy(cfg.webFontChoice, doc["webFontChoice"] | "inter", sizeof(cfg.webFontChoice));

  strlcpy(cfg.flashLightIds, doc["flashLightIds"] | "", sizeof(cfg.flashLightIds));
  cfg.flashPulseRateMs = doc["flashPulseRateMs"] | 500;
  cfg.flashPulseCount = doc["flashPulseCount"] | 5;
  cfg.flashBrightnessPct = constrain((int)(doc["flashBrightnessPct"] | 25), 1, 100);
  cfg.flashOnExpire = doc["flashOnExpire"] | true;
  cfg.marqueeEnabled = doc["marqueeEnabled"] | true;

  {
    JsonArray presetsSecArr = doc["timerPresetSec"].as<JsonArray>();
    JsonArray legacyMinArr = doc["timerPresetMin"].as<JsonArray>(); // pre-seconds-support exports
    for (int i = 0; i < 5; i++) {
      if (i < (int)presetsSecArr.size()) {
        cfg.timerPresetSec[i] = presetsSecArr[i].as<int>();
      } else if (i < (int)legacyMinArr.size()) {
        cfg.timerPresetSec[i] = legacyMinArr[i].as<int>() * 60;
      } else {
        cfg.timerPresetSec[i] = DEFAULT_TIMER_PRESETS_SEC[i];
      }
      if (cfg.timerPresetSec[i] < 1) cfg.timerPresetSec[i] = DEFAULT_TIMER_PRESETS_SEC[i];
    }
  }

  cfg.customPageOrder = doc["customPageOrder"] | false;

  cfg.useStaticIp = doc["useStaticIp"] | false;
  strlcpy(cfg.ipAddr,  doc["ipAddr"]  | "", sizeof(cfg.ipAddr));
  strlcpy(cfg.subnet,  doc["subnet"]  | "", sizeof(cfg.subnet));
  strlcpy(cfg.gateway, doc["gateway"] | "", sizeof(cfg.gateway));
  strlcpy(cfg.dns1,    doc["dns1"]    | "", sizeof(cfg.dns1));
  strlcpy(cfg.dns2,    doc["dns2"]    | "", sizeof(cfg.dns2));

  JsonArray pagesArr = doc["pages"].as<JsonArray>();
  cfg.pageCount = 0;
  for (JsonObject p : pagesArr) {
    if (cfg.pageCount >= MAX_PAGES) break;
    PageConfig& pg = cfg.pages[cfg.pageCount];
    strlcpy(pg.id, p["id"] | "", sizeof(pg.id));
    strlcpy(pg.name, p["name"] | "Page", sizeof(pg.name));
    strlcpy(pg.type, p["type"] | "area", sizeof(pg.type));
    pg.deletable = p["deletable"] | true;
    pg.hidden = (p["hidden"] | false) && pageCanHide(pg.type);
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
      tl.dateEuro = t["date_euro"] | false;
      pg.tileCount++;
    }
    cfg.pageCount++;
  }

  if (cfg.pageCount == 0) return false;
  if (!cfg.customPageOrder) sortPages(); // otherwise keep the user's drag order
  return true;
}

void buildConfigJson(JsonDocument& doc) {
  doc["deviceName"] = cfg.deviceName;
  doc["haUrl"] = cfg.haUrl;
  doc["haToken"] = cfg.haToken;
  doc["haLiveUpdates"] = cfg.haLiveUpdates;
  doc["darkTheme"] = cfg.darkTheme;
  doc["use12Hour"] = cfg.use12Hour;
  doc["flipScreen"] = cfg.flipScreen;
  doc["uiFontSize"] = cfg.uiFontSize;
  doc["uiTypeface"] = cfg.uiTypeface;
  doc["colorScheme"] = cfg.colorScheme;
  doc["cornerStyle"] = cfg.cornerStyle;
  doc["uiBoldText"] = cfg.uiBoldText;
  doc["bulbColorKey"] = cfg.bulbColorKey;
  doc["timezone"] = cfg.timezone;
  doc["weatherLat"] = cfg.weatherLat;
  doc["weatherLon"] = cfg.weatherLon;
  doc["weatherLocationName"] = cfg.weatherLocationName;
  doc["webFontChoice"] = cfg.webFontChoice;
  doc["flashLightIds"] = cfg.flashLightIds;
  doc["flashPulseRateMs"] = cfg.flashPulseRateMs;
  doc["flashPulseCount"] = cfg.flashPulseCount;
  doc["flashBrightnessPct"] = cfg.flashBrightnessPct;
  doc["flashOnExpire"] = cfg.flashOnExpire;
  doc["marqueeEnabled"] = cfg.marqueeEnabled;

  JsonArray presetsOut = doc.createNestedArray("timerPresetSec");
  for (int i = 0; i < 5; i++) presetsOut.add(cfg.timerPresetSec[i]);
  doc["customPageOrder"] = cfg.customPageOrder;
  doc["useStaticIp"] = cfg.useStaticIp;
  doc["ipAddr"] = cfg.ipAddr;
  doc["subnet"] = cfg.subnet;
  doc["gateway"] = cfg.gateway;
  doc["dns1"] = cfg.dns1;
  doc["dns2"] = cfg.dns2;

  JsonArray pagesArr = doc.createNestedArray("pages");
  for (int i = 0; i < cfg.pageCount; i++) {
    PageConfig& pg = cfg.pages[i];
    JsonObject p = pagesArr.createNestedObject();
    p["id"] = pg.id;
    p["name"] = pg.name;
    p["type"] = pg.type;
    p["deletable"] = pg.deletable;
    p["hidden"] = pg.hidden;

    JsonArray tilesArr = p.createNestedArray("tiles");
    for (int j = 0; j < pg.tileCount; j++) {
      TileConfig& tl = pg.tiles[j];
      JsonObject t = tilesArr.createNestedObject();
      t["type"] = tl.type;
      t["label"] = tl.label;
      t["entity_id"] = tl.entityId;
      t["size"] = tl.size;
      t["date_euro"] = tl.dateEuro;
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
bool sliderOverlayShown = false; // the big centred brightness slider is on screen
unsigned long touchStartMs = 0;
int touchStartX = 0, touchStartY = 0;
int sliderPercent = 0;
int sliderAnchorY = 0;    // touch Y when the slider engaged
int sliderAnchorPct = 0;  // brightness at that moment - drag is relative to here
bool sliderDragged = false; // finger actually moved after the overlay appeared

// A tile tap queues its HA call here instead of making the (synchronous,
// ~100-400 ms) HTTP request inline - that stall used to land right in the
// release-debounce window and let contact bounce double-toggle the tile.
// loop() drains this one slot right after the touch pass.
enum PendingHaKind { PHA_NONE, PHA_ONOFF, PHA_ACTIVATE, PHA_BRIGHTNESS };
PendingHaKind pendingHaKind = PHA_NONE;
char pendingHaEntity[48] = "";
bool pendingHaOn = false;
int  pendingHaPct = 0;

void queueHaOnOff(const char* entityId, bool on) {
  strlcpy(pendingHaEntity, entityId, sizeof(pendingHaEntity));
  pendingHaOn = on;
  pendingHaKind = PHA_ONOFF;
}
void queueHaActivate(const char* entityId) {
  strlcpy(pendingHaEntity, entityId, sizeof(pendingHaEntity));
  pendingHaKind = PHA_ACTIVATE;
}
void queueHaBrightness(const char* entityId, int pct) {
  strlcpy(pendingHaEntity, entityId, sizeof(pendingHaEntity));
  pendingHaPct = pct;
  pendingHaKind = PHA_BRIGHTNESS;
}

// Horizontal swipe tracking (separate from the tile long-press tracker;
// only armed when a press starts outside any tile, i.e. on empty grid
// space, so it doesn't fight with tile taps/holds).
bool swipeCandidate = false;
int swipeStartX = 0, swipeStartY = 0;

const unsigned long LONG_PRESS_MS = 450;
const int MOVE_TOLERANCE = 20; // resistive touch has some inherent jitter; too tight and taps/holds misfire
// While dragging the brightness slider the resistive panel's pressure
// reading dips in and out - tolerate brief contact loss so a moving finger
// isn't misread as "released".
const unsigned long SLIDER_RELEASE_GRACE_MS = 130;
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
// The Timers-page "Flash Lights" toggle is now persisted (cfg.flashOnExpire),
// defaulting on; the tap handler writes it through to config.

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

// Flashing red border around the whole screen - only once a timer has
// EXPIRED (nothing while it's counting down), and only if cfg.marqueeEnabled.
// It keeps blinking until Stop/Restart clears timerExpired. Re-asserted on
// top of whatever page is showing every loop() tick rather than reserving
// permanent screen space.
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
  bool active = timerExpired && cfg.marqueeEnabled;

  if (!active) {
    if (marqueeCurrentlyDrawn) {
      pageDirty = true; // let the normal page redraw clean up the border
      marqueeCurrentlyDrawn = false;
    }
    return;
  }

  unsigned long now = millis();
  if (now - lastMarqueeToggleMs >= MARQUEE_BLINK_MS) {
    marqueeVisible = !marqueeVisible;
    lastMarqueeToggleMs = now;
  }
  bool visible = marqueeVisible;

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
    if (cfg.flashOnExpire) startFlashSequence();
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
int weatherSunriseMin = -1;     // today's sunrise, minutes since local midnight (-1 = unknown)
int weatherSunsetMin = -1;
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

// minutes-since-midnight -> "6:12 AM" / "06:12" per cfg.use12Hour.
String formatClockMin(int minsSinceMidnight) {
  if (minsSinceMidnight < 0) return "--:--";
  int hh = (minsSinceMidnight / 60) % 24;
  int mm = minsSinceMidnight % 60;
  char buf[12];
  if (cfg.use12Hour) {
    int h12 = hh % 12;
    if (h12 == 0) h12 = 12;
    snprintf(buf, sizeof(buf), "%d:%02d %s", h12, mm, hh >= 12 ? "PM" : "AM");
  } else {
    snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
  }
  return String(buf);
}

bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure(); // no cert store on-device; Open-Meteo has no sensitive data
  HTTPClient http;

  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(cfg.weatherLat, 4) +
               "&longitude=" + String(cfg.weatherLon, 4) +
               "&current=temperature_2m,weather_code,is_day" +
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset" +
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

  DynamicJsonDocument doc(5120);
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

  // Today's sunrise/sunset come back as local ISO strings ("2026-08-30T06:12")
  // because we request timezone=auto. Keep just minutes-since-midnight so the
  // tiles can re-render in 12/24h without a refetch.
  {
    JsonArray sr = doc["daily"]["sunrise"].as<JsonArray>();
    JsonArray ss = doc["daily"]["sunset"].as<JsonArray>();
    auto isoToMin = [](const String& s) -> int {
      if (s.length() < 16) return -1;
      return s.substring(11, 13).toInt() * 60 + s.substring(14, 16).toInt();
    };
    weatherSunriseMin = (sr.size() > 0) ? isoToMin(sr[0].as<String>()) : -1;
    weatherSunsetMin  = (ss.size() > 0) ? isoToMin(ss[0].as<String>()) : -1;
  }

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
  if (sliderOverlayShown) return; // the fetch blocks - don't stutter an active drag

  time_t nowT = time(nullptr);
  if (weatherKnown && (nowT - lastWeatherFetch) < (time_t)WEATHER_INTERVAL_SEC) return;

  if (fetchWeather()) {
    for (int p = 0; p < cfg.pageCount; p++) {
      for (int t = 0; t < cfg.pages[p].tileCount; t++) {
        const char* ty = cfg.pages[p].tiles[t].type;
        if (strcmp(ty, "weather") == 0 || strcmp(ty, "sunrise") == 0 ||
            strcmp(ty, "sunset") == 0 || strcmp(ty, "sun") == 0) {
          tileRuntime[p][t].cacheKey = "";
        }
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

  uint16_t bg = (cutColor == 0x0000) ? COL_BG : cutColor;

  if (code == 0) {
    if (isDay) {
      // Clear / sunny
      uiFillCircle(d, cx, cy, S(9), sunColor);
      d.drawWideLine(cx, cy - S(15), cx, cy - S(11), 1.4f, sunColor, bg);
      d.drawWideLine(cx, cy + S(11), cx, cy + S(15), 1.4f, sunColor, bg);
      d.drawWideLine(cx - S(15), cy, cx - S(11), cy, 1.4f, sunColor, bg);
      d.drawWideLine(cx + S(11), cy, cx + S(15), cy, 1.4f, sunColor, bg);
      d.drawWideLine(cx - S(11), cy - S(11), cx - S(8), cy - S(8), 1.4f, sunColor, bg);
      d.drawWideLine(cx + S(11), cy - S(11), cx + S(8), cy - S(8), 1.4f, sunColor, bg);
      d.drawWideLine(cx - S(11), cy + S(11), cx - S(8), cy + S(8), 1.4f, sunColor, bg);
      d.drawWideLine(cx + S(11), cy + S(11), cx + S(8), cy + S(8), 1.4f, sunColor, bg);
    } else {
      // Clear / night - crescent moon: a filled disc with a second disc,
      // in the background color, punched out of one side.
      uiFillCircle(d, cx, cy, S(9), sunColor);
      uiFillCircle(d, cx + S(5), cy - S(3), S(8), bg);
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
    uiFillCircle(d, cx - S(7), cloudCy - S(7), S(6), sunColor); // sun/moon peeking out for partly-cloudy
    if (!isDay) {
      uiFillCircle(d, cx - S(4), cloudCy - S(9), S(5), bg); // crescent cutout
    }
  }

  // Cloud shape - base for partly-cloudy, overcast, rain, snow, storm.
  // Filled first, then each bump's own outline drawn on top - the
  // outline is what actually reads as "cloud" rather than a blob, since
  // three same-color filled circles alone merge into one shapeless mass.
  uiFillCircle(d, cx - S(6), cloudCy, S(7), cloudFill);
  uiFillCircle(d, cx + S(5), cloudCy, S(8), cloudFill);
  uiFillCircle(d, cx, cloudCy - S(4), S(7), cloudFill);
  uiFillRR(d, cx - S(10), cloudCy, S(22), S(8), S(4), cloudFill, bg);
  d.drawSmoothCircle(cx - S(6), cloudCy, S(7), cloudStroke, cloudFill);
  d.drawSmoothCircle(cx + S(5), cloudCy, S(8), cloudStroke, cloudFill);
  d.drawSmoothCircle(cx, cloudCy - S(4), S(7), cloudStroke, cloudFill);
  d.drawFastHLine(cx - S(10), cloudCy + S(8), S(22), cloudStroke); // flat base edge

  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    // Rain
    for (int i = -1; i <= 1; i++) {
      d.drawWideLine(cx + i * S(7), cloudCy + S(9), cx + i * S(7) - S(2), cloudCy + S(15), 1.4f, rainColor, bg);
    }
  } else if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) {
    // Snow
    for (int i = -1; i <= 1; i++) {
      uiFillCircle(d, cx + i * S(7), cloudCy + S(12), max(1, S(1)), cloudFill);
    }
  } else if (code >= 95) {
    // Thunderstorm - simple bolt
    d.fillTriangle(cx - S(2), cloudCy + S(8), cx + S(4), cloudCy + S(8), cx - S(1), cloudCy + S(16), sunColor);
    d.fillTriangle(cx - S(1), cloudCy + S(16), cx + S(5), cloudCy + S(12), cx + S(2), cloudCy + S(20), sunColor);
  }
}

// Simple clock face for the Timer tile type.
template<typename T>
void drawClockIcon(T& d, int cx, int cy, uint16_t iconColor, float scale = 1.0f, uint16_t bg = COL_BG) {
  auto S = [scale](int v) { return (int)roundf(v * scale); };
  int r = S(12);

  d.drawSmoothCircle(cx, cy, r, iconColor, bg);

  // Small tick marks at 12/3/6/9, like a clean SF Symbols-style clock
  // face, instead of a bare outline.
  int tickLen = S(2);
  d.drawFastVLine(cx, cy - r, tickLen, iconColor);
  d.drawFastVLine(cx, cy + r - tickLen, tickLen, iconColor);
  d.drawFastHLine(cx - r, cy, tickLen, iconColor);
  d.drawFastHLine(cx + r - tickLen, cy, tickLen, iconColor);

  // Hands with a small filled hub, drawn as anti-aliased wide lines.
  d.drawWideLine(cx, cy, cx, cy - S(8), 1.6f, iconColor, bg);
  d.drawWideLine(cx, cy, cx + S(6), cy + S(2), 1.6f, iconColor, bg);
  uiFillCircle(d, cx, cy, S(2), iconColor);
}

// =========================================================
// PAGE-TYPE HEADER ICONS
// =========================================================
// Outline-only, single-color icons (same visual weight as drawClockIcon
// above) so they render correctly in both themes with one color. Sized so
// scale 1.0 is ~24px tall, matching the clock icon; the header calls them
// smaller. Templated on the drawing target like the weather/clock icons.

template<typename T>
void drawNavBulbIcon(T& d, int cx, int cy, uint16_t c, float scale = 1.0f, uint16_t bg = COL_BG) {
  auto S = [scale](int v) { return (int)roundf(v * scale); };
  int r = S(8);
  d.drawSmoothCircle(cx, cy - S(2), r, c, bg); // glass
  // screw base - a few narrowing rungs under the glass
  d.drawFastHLine(cx - S(4), cy + S(6), S(8), c);
  d.drawFastHLine(cx - S(4), cy + S(8), S(8), c);
  d.drawFastHLine(cx - S(3), cy + S(10), S(6), c);
  d.drawWideLine(cx - S(5), cy + S(4), cx - S(4), cy + S(6), 1.3f, c, bg);
  d.drawWideLine(cx + S(5), cy + S(4), cx + S(4), cy + S(6), 1.3f, c, bg);
}

template<typename T>
void drawCloudIcon(T& d, int cx, int cy, uint16_t c, float scale = 1.0f, uint16_t bg = COL_BG) {
  auto S = [scale](int v) { return (int)roundf(v * scale); };
  d.drawSmoothCircle(cx - S(5), cy + S(1), S(5), c, bg);
  d.drawSmoothCircle(cx + S(4), cy + S(1), S(6), c, bg);
  d.drawSmoothCircle(cx, cy - S(3), S(5), c, bg);
  d.drawFastHLine(cx - S(9), cy + S(6), S(19), c); // flat base ties the puffs together
}

template<typename T>
void drawGearIcon(T& d, int cx, int cy, uint16_t c, float scale = 1.0f, uint16_t bg = COL_BG) {
  auto S = [scale](int v) { return (int)roundf(v * scale); };
  int r = S(6);
  d.drawSmoothCircle(cx, cy, r, c, bg);       // body
  d.drawSmoothCircle(cx, cy, S(2), c, bg);    // hub hole
  // 8 teeth as short thick spokes, using fixed unit vectors (no trig)
  static const float dirs[8][2] = {
    {1, 0}, {-1, 0}, {0, 1}, {0, -1},
    {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}
  };
  int tooth = S(3);
  for (int i = 0; i < 8; i++) {
    int x1 = cx + (int)roundf(dirs[i][0] * r);
    int y1 = cy + (int)roundf(dirs[i][1] * r);
    int x2 = cx + (int)roundf(dirs[i][0] * (r + tooth));
    int y2 = cy + (int)roundf(dirs[i][1] * (r + tooth));
    d.drawWideLine(x1, y1, x2, y2, 1.8f, c, bg);
  }
}

// Dispatches on PageConfig::type. "home" and user-added "area" pages get
// the bulb; unknown types fall back to the bulb too.
template<typename T>
void drawPageTypeIcon(T& d, const char* type, int cx, int cy, uint16_t c, float scale = 1.0f, uint16_t bg = COL_BG) {
  if (strcmp(type, "forecast") == 0)     drawCloudIcon(d, cx, cy, c, scale, bg);
  else if (strcmp(type, "timers") == 0)  drawClockIcon(d, cx, cy, c, scale, bg);
  else if (strcmp(type, "status") == 0)  drawGearIcon(d, cx, cy, c, scale, bg);
  else                                   drawNavBulbIcon(d, cx, cy, c, scale, bg);
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

  // Touch stays on its rotation-2 calibration; when the display is flipped
  // 180 (cfg.flipScreen) the whole panel is physically upside down, so the
  // touch point is a 180 rotation of what the user means - undo it here.
  if (cfg.flipScreen) {
    x = SCREEN_W - 1 - x;
    y = SCREEN_H - 1 - y;
  }
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

// One-shot "activate" for stateless tiles (scene / script / button). Picks
// the right service from the entity's domain.
bool haActivate(const char* entityId) {
  if (!haConfigured() || WiFi.status() != WL_CONNECTED) return false;
  String domain = entityDomain(entityId);
  if (domain.length() == 0) return false;

  String service;
  if (domain == "scene") service = "scene/turn_on";
  else if (domain == "script") service = "script/turn_on";
  else if (domain == "button" || domain == "input_button") service = domain + "/press";
  else service = domain + "/turn_on"; // best effort for anything else

  WiFiClient client;
  HTTPClient http;
  String url = String(cfg.haUrl) + "/api/services/" + service;
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

// Applies one HA state object ({"state":..,"attributes":{..}}) to a tile
// runtime. Shared by the REST poll and the WebSocket live-update path -
// both receive the same shape (REST /api/states/<id>, WS trigger to_state).
void applyEntityStateJson(JsonVariantConst st, TileRuntime& rt) {
  const char* state = st["state"];
  if (!state) return;

  rt.lastRawState = String(state);
  rt.unavailable = (rt.lastRawState == "unavailable" || rt.lastRawState == "unknown");
  rt.on = (rt.lastRawState == "on");
  rt.known = true;

  JsonVariantConst brightnessVar = st["attributes"]["brightness"];
  if (!brightnessVar.isNull()) {
    int b255 = brightnessVar.as<int>();
    rt.brightnessPct = constrain((int)((b255 * 100L + 127L) / 255L), 1, 100);
  } else if (rt.on) {
    rt.brightnessPct = 100;
  }
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
  if (!doc["state"]) return false;

  applyEntityStateJson(doc.as<JsonVariantConst>(), rt);
  return true;
}

// =========================================================
// HA CONNECTION STATE / DISCOVERY / CONFIG SYNC
// =========================================================
// enum HaConnState lives in config_types.h (auto-prototype ordering).
HaConnState haConnState = HA_CONN_UNKNOWN;
unsigned long lastHaCheckMs = 0;
const unsigned long HA_CHECK_INTERVAL_MS = 60UL * 1000UL;

const char* haConnLabel(HaConnState s) {
  switch (s) {
    case HA_CONN_OK:           return "Connected";
    case HA_CONN_AUTH_FAIL:    return "Auth failed";
    case HA_CONN_UNREACHABLE:  return "Unreachable";
    case HA_CONN_UNCONFIGURED: return "Not set up";
    default:                   return "Checking...";
  }
}

// GET /api/ with the saved token - HA's lightweight "is the API alive and
// is this token valid" probe. Separates auth failure from unreachable so
// the UI can point the user at the right field.
HaConnState haProbeConnection() {
  if (strlen(cfg.haUrl) == 0 || strlen(cfg.haToken) == 0) return HA_CONN_UNCONFIGURED;
  if (WiFi.status() != WL_CONNECTED) return HA_CONN_UNREACHABLE;

  WiFiClient client;
  HTTPClient http;
  String url = String(cfg.haUrl) + "/api/";
  if (!http.begin(client, url)) return HA_CONN_UNREACHABLE;
  http.addHeader("Authorization", "Bearer " + String(cfg.haToken));
  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  int code = http.GET();
  http.end();

  if (code == 200) return HA_CONN_OK;
  if (code == 401 || code == 403) return HA_CONN_AUTH_FAIL;
  return HA_CONN_UNREACHABLE;
}

// Called from loop(). Re-probes on a slow interval; only forces a redraw
// when the state changes while the Status page is showing.
void ensureHaCheck() {
  if (sliderOverlayShown) return; // the probe blocks - don't stutter an active drag
  if (haConnState != HA_CONN_UNKNOWN && millis() - lastHaCheckMs < HA_CHECK_INTERVAL_MS) return;
  lastHaCheckMs = millis();
  HaConnState s = haProbeConnection();
  if (s != haConnState) {
    haConnState = s;
    if (strcmp(cfg.pages[currentPageIndex].type, "status") == 0) pageDirty = true;
  }
}

// Maps HA's IANA time-zone name (e.g. "America/Los_Angeles") to this
// project's own short tz key. Returns "" when we carry no matching zone -
// the caller then leaves the user's setting alone.
String tzKeyFromIana(const String& iana) {
  struct { const char* iana; const char* key; } M[] = {
    {"America/Los_Angeles", "us_pacific"}, {"America/Vancouver", "us_pacific"},
    {"America/Denver", "us_mountain"},     {"America/Edmonton", "us_mountain"},
    {"America/Phoenix", "us_arizona"},
    {"America/Chicago", "us_central"},     {"America/Winnipeg", "us_central"},
    {"America/New_York", "us_eastern"},    {"America/Toronto", "us_eastern"},
    {"America/Anchorage", "alaska"},
    {"Pacific/Honolulu", "hawaii"},
    {"UTC", "utc"}, {"Etc/UTC", "utc"},
    {"Europe/London", "uk"}, {"Europe/Dublin", "uk"},
    {"Europe/Berlin", "europe_central"},   {"Europe/Paris", "europe_central"},
    {"Europe/Madrid", "europe_central"},    {"Europe/Rome", "europe_central"},
    {"Europe/Amsterdam", "europe_central"}, {"Europe/Brussels", "europe_central"},
    {"Asia/Kolkata", "india"}, {"Asia/Calcutta", "india"},
    {"Asia/Tokyo", "asia_tokyo"},
    {"Australia/Sydney", "australia_sydney"}, {"Australia/Melbourne", "australia_sydney"},
  };
  for (auto& m : M) if (iana == m.iana) return m.key;
  return "";
}

// GET /api/config after a good token save. Uses a deserialization filter
// because the full payload (component list) is large. Applies the time
// zone (mapped to our key set) and the exact lat/lon + name, switching
// weather off "auto" since HA's coordinates beat a tz-derived city centre.
// Any HTTP or parse failure leaves every setting untouched.
bool haFetchAndApplyConfig() {
  if (strlen(cfg.haUrl) == 0 || strlen(cfg.haToken) == 0) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  String url = String(cfg.haUrl) + "/api/config";
  if (!http.begin(client, url)) return false;
  http.addHeader("Authorization", "Bearer " + String(cfg.haToken));
  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();

  StaticJsonDocument<192> filter;
  filter["time_zone"] = true;
  filter["latitude"] = true;
  filter["longitude"] = true;
  filter["location_name"] = true;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;

  bool changed = false;

  const char* tz = doc["time_zone"];
  if (tz) {
    String key = tzKeyFromIana(String(tz));
    if (key.length() > 0 && key != cfg.timezone) {
      strlcpy(cfg.timezone, key.c_str(), sizeof(cfg.timezone));
      applyTimezone();
      changed = true;
    }
  }

  if (!doc["latitude"].isNull() && !doc["longitude"].isNull()) {
    cfg.weatherLat = doc["latitude"].as<float>();
    cfg.weatherLon = doc["longitude"].as<float>();
    // Only seed the display name from HA when the user hasn't set their own
    // - HA's "location_name" is the instance name (often "Home"), not a
    // city, and it shouldn't clobber what the user typed on the Weather card.
    const char* ln = doc["location_name"];
    if (ln && strlen(ln) > 0 && strlen(cfg.weatherLocationName) == 0) {
      strlcpy(cfg.weatherLocationName, ln, sizeof(cfg.weatherLocationName));
    }
    weatherKnown = false;
    lastWeatherFetch = 0;
    changed = true;
  }

  return changed;
}

// Best-effort LAN discovery of HA via its zeroconf advert
// (_home-assistant._tcp). Only used to *suggest* a URL when none is set -
// never overrides a saved value. Many networks block mDNS, so a failure
// here is normal and silent.
bool discoverHaUrl(String& out) {
  int n = MDNS.queryService("home-assistant", "tcp");
  if (n <= 0) return false;

  IPAddress ip = MDNS.IP(0);
  uint16_t port = MDNS.port(0);
  if (ip == IPAddress((uint32_t)0)) return false;
  if (port == 0) port = 8123;
  out = "http://" + ip.toString() + ":" + String(port);
  return true;
}

// POST /api/template - renders a Jinja template server-side and returns the
// text. Used for entity/area discovery so the ESP32 never has to hold or
// parse HA's full state list; callers keep templates to short "id|name"
// lines. Returns false (out untouched) on any HTTP/parse failure.
bool haRenderTemplate(const String& tmpl, String& out) {
  if (strlen(cfg.haUrl) == 0 || strlen(cfg.haToken) == 0) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  String url = String(cfg.haUrl) + "/api/template";
  if (!http.begin(client, url)) return false;
  http.addHeader("Authorization", "Bearer " + String(cfg.haToken));
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(3000);
  http.setTimeout(6000);

  DynamicJsonDocument doc(tmpl.length() + 128);
  doc["template"] = tmpl;
  String reqBody;
  serializeJson(doc, reqBody);

  int code = http.POST(reqBody);
  if (code != 200) { http.end(); return false; }
  out = http.getString();
  http.end();
  return true;
}

// entity_id / area_id fragments we splice into a template must be plain
// slugs - reject anything else rather than build a broken template.
bool isSafeSlug(const String& s) {
  if (s.length() == 0 || s.length() > 64) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
  }
  return true;
}

// "light.office_ceiling" -> "Office Ceiling" - fallback tile label when HA
// gives us no friendly name.
String prettyFromEntityId(const String& eid) {
  int dot = eid.indexOf('.');
  String s = dot >= 0 ? eid.substring(dot + 1) : eid;
  s.replace('_', ' ');
  bool cap = true;
  for (size_t i = 0; i < s.length(); i++) {
    if (cap && isalpha((unsigned char)s[i])) { s[i] = toupper(s[i]); cap = false; }
    else if (s[i] == ' ') cap = true;
  }
  return s;
}

// =========================================================
// HA WEBSOCKET LIVE UPDATES  (optional - cfg.haLiveUpdates, off by default)
// =========================================================
// When enabled and the HA URL is plain http:// (so ws://), hold a
// WebSocket to HA and push state changes straight into the tiles instead
// of polling. Anything that goes wrong (no connect, bad token, https URL)
// falls back to REST polling, which also stays on as a slow backstop.
//
// subscribe_entities (HA 2022.4+) with an explicit entity list (the union
// of every light/switch/sensor tile across all pages): the socket carries
// only our entities, HA sends the full initial state on subscribe, and
// every later change - including transitions to "unavailable" - arrives as
// a compact a/c (added/changed) diff. How fast "unavailable" shows up
// after a device loses power is entirely down to HA's own availability
// timeout for that integration, not this code.
WebSocketsClient haWs;
enum HaWsPhase { HAWS_OFF, HAWS_CONNECTING, HAWS_AUTH, HAWS_READY, HAWS_FAILED };
HaWsPhase haWsPhase = HAWS_OFF;
int haWsSubId = 1;
unsigned long haWsLastRxMs = 0;
uint32_t haWsEntitySig = 0;      // FNV hash of the subscribed entity set
unsigned long haWsSigCheckMs = 0;
unsigned long haWsEventCount = 0; // diagnostics
int haWsDropCount = 0;           // socket disconnects since boot
String haWsNote = "";            // last human-readable status detail

bool haWsUrlSupported() { return strncmp(cfg.haUrl, "http://", 7) == 0; }
bool haWsActive() { return haWsPhase == HAWS_READY; }

const char* haWsStatusText() {
  if (!cfg.haLiveUpdates)       return "Off";
  switch (haWsPhase) {
    case HAWS_READY:      return "Live (WebSocket)";
    case HAWS_CONNECTING: return "Connecting...";
    case HAWS_AUTH:       return "Authenticating...";
    case HAWS_FAILED:     return "Unavailable - polling";
    default:              return "Starting...";
  }
}

// Every distinct light/switch/sensor entity id across all pages.
int haWsCollectEntities(String out[], int maxN) {
  int n = 0;
  for (int p = 0; p < cfg.pageCount && n < maxN; p++) {
    for (int t = 0; t < cfg.pages[p].tileCount && n < maxN; t++) {
      TileConfig& tl = cfg.pages[p].tiles[t];
      if (strlen(tl.entityId) == 0) continue;
      if (strcmp(tl.type, "light") && strcmp(tl.type, "switch") && strcmp(tl.type, "sensor")) continue;
      bool dup = false;
      for (int i = 0; i < n; i++) if (out[i] == tl.entityId) { dup = true; break; }
      if (!dup) out[n++] = tl.entityId;
    }
  }
  return n;
}

uint32_t haWsEntitySignature() {
  String ents[MAX_PAGES * MAX_TILES];
  int n = haWsCollectEntities(ents, MAX_PAGES * MAX_TILES);
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++)
    for (size_t j = 0; j < ents[i].length(); j++) h = (h ^ (uint8_t)ents[i][j]) * 16777619u;
  return h ^ (uint32_t)n;
}

bool haWsRawLog = false; // set true to dump raw frames to Serial for debugging

void haWsSendSubscribe() {
  String ents[MAX_PAGES * MAX_TILES];
  int n = haWsCollectEntities(ents, MAX_PAGES * MAX_TILES);
  haWsEntitySig = haWsEntitySignature();
  if (n == 0) return;

  String msg = "{\"id\":" + String(haWsSubId++) + ",\"type\":\"subscribe_entities\",\"entity_ids\":[";
  for (int i = 0; i < n; i++) { if (i) msg += ","; msg += "\"" + ents[i] + "\""; }
  msg += "]}";
  haWs.sendTXT(msg);
  Serial.printf("[haWs] subscribe_entities: %d entities\n", n);
}

// One entity's slice of a subscribe_entities event. `full` = from the
// initial "a" (added) map, a complete state; otherwise it's a "c" (changed)
// diff with the new/changed bits nested under "+".
//   full:  {"s":"on","a":{"brightness":128,...},"lc":..,"lu":..,"c":..}
//   diff:  {"+":{"s":"off","a":{...}}, "-":{...}}
void haWsApplyEntitySlice(const char* eid, JsonVariantConst slice, bool full) {
  JsonVariantConst payload = full ? slice : slice["+"];
  JsonVariantConst s  = payload["s"];               // new state string (may be absent)
  JsonVariantConst br = payload["a"]["brightness"]; // new brightness (may be absent)
  if (s.isNull() && br.isNull()) return;

  int hits = 0;
  for (int p = 0; p < cfg.pageCount; p++) {
    for (int t = 0; t < cfg.pages[p].tileCount; t++) {
      if (strcmp(cfg.pages[p].tiles[t].entityId, eid) != 0) continue;
      TileRuntime& rt = tileRuntime[p][t];
      if (!s.isNull()) {
        rt.lastRawState = String((const char*)s);
        rt.unavailable = (rt.lastRawState == "unavailable" || rt.lastRawState == "unknown");
        rt.on = (rt.lastRawState == "on");
        rt.known = true;
      }
      if (!br.isNull()) {
        int b255 = br.as<int>();
        rt.brightnessPct = constrain((int)((b255 * 100L + 127L) / 255L), 1, 100);
      } else if (full && rt.on) {
        rt.brightnessPct = 100; // "on" with no brightness attribute = non-dimmable
      }
      rt.cacheKey = "";
      hits++;
    }
  }
  if (hits) haWsEventCount++;
  Serial.printf("[haWs] %s%s -> %s (%d tiles)\n", eid, full ? " [full]" : "",
                s.isNull() ? "brightness" : (const char*)s, hits);
}

void haWsHandleEvent(JsonVariantConst ev) {
  JsonObjectConst added = ev["a"];   // initial full states
  for (JsonPairConst kv : added) haWsApplyEntitySlice(kv.key().c_str(), kv.value(), true);
  JsonObjectConst changed = ev["c"]; // subsequent diffs
  for (JsonPairConst kv : changed) haWsApplyEntitySlice(kv.key().c_str(), kv.value(), false);
}

void haWsHandleText(uint8_t* payload, size_t len) {
  haWsLastRxMs = millis();
  if (haWsRawLog) {
    size_t show = len > 320 ? 320 : len;
    Serial.printf("[haWs] rx %ub: ", (unsigned)len);
    Serial.write(payload, show);
    if (show < len) Serial.print(" ...");
    Serial.println();
  }

  // ONE zero-copy parse. (An earlier "peek at type first" pass also parsed
  // zero-copy over the same buffer, which mangled it for this pass - hence
  // the parse failures.) subscribe_entities slices have dynamic entity-id
  // keys so a key filter can't help; size the doc to the frame instead.
  size_t cap = len * 2 + 768;
  if (cap > 28000) cap = 28000;
  DynamicJsonDocument doc(cap);
  DeserializationError err = deserializeJson(doc, payload, len);
  if (err) {
    Serial.printf("[haWs] parse failed: %s (frame %ub, cap %u, heap %u)\n",
                  err.c_str(), (unsigned)len, (unsigned)cap, (unsigned)ESP.getFreeHeap());
    return;
  }

  const char* type = doc["type"];
  if (!type) return;

  if (strcmp(type, "auth_required") == 0) {
    haWs.sendTXT(String("{\"type\":\"auth\",\"access_token\":\"") + cfg.haToken + "\"}");
    haWsPhase = HAWS_AUTH;
  } else if (strcmp(type, "auth_ok") == 0) {
    Serial.println("[haWs] authenticated");
    haWsNote = "";
    haWsPhase = HAWS_READY;
    haWsSendSubscribe();
  } else if (strcmp(type, "auth_invalid") == 0) {
    Serial.println("[haWs] token rejected");
    haWsNote = "token rejected";
    haWsPhase = HAWS_FAILED;
    haWs.disconnect();
  } else if (strcmp(type, "event") == 0) {
    haWsHandleEvent(doc["event"]);
  }
}

void haWsOnEvent(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[haWs] socket connected (%s)\n", (const char*)payload);
      haWsNote = "socket up, authenticating";
      haWsPhase = HAWS_AUTH;
      haWsLastRxMs = millis();
      break;
    case WStype_DISCONNECTED:
      haWsDropCount++;
      Serial.printf("[haWs] disconnected (#%d)\n", haWsDropCount);
      haWsNote = "retrying";
      if (haWsPhase == HAWS_READY || haWsPhase == HAWS_AUTH) haWsPhase = HAWS_CONNECTING;
      break;
    case WStype_TEXT:
      haWsHandleText(payload, len);
      break;
    case WStype_ERROR:
      Serial.printf("[haWs] error: %s\n", (const char*)payload);
      haWsNote = "socket error";
      break;
    case WStype_PING:
    case WStype_PONG:
      break;
    default:
      break;
  }
}

void haWsStop() {
  if (haWsPhase == HAWS_OFF) return;
  haWs.disconnect();
  haWsPhase = HAWS_OFF;
  Serial.println("[haWs] stopped");
}

void haWsBegin() {
  if (haWsPhase != HAWS_OFF && haWsPhase != HAWS_FAILED) return;
  if (!cfg.haLiveUpdates || !haConfigured()) return;
  if (!haWsUrlSupported()) {
    Serial.println("[haWs] HA URL is https:// - live updates need ws://, using polling");
    haWsNote = "HA URL is https:// (needs http://)";
    haWsPhase = HAWS_FAILED;
    return;
  }

  String u = String(cfg.haUrl).substring(7); // strip "http://"
  int slash = u.indexOf('/');
  if (slash >= 0) u = u.substring(0, slash);
  int colon = u.indexOf(':');
  String host = colon >= 0 ? u.substring(0, colon) : u;
  int port = colon >= 0 ? u.substring(colon + 1).toInt() : 8123;

  Serial.printf("[haWs] connecting to ws://%s:%d/api/websocket\n", host.c_str(), port);
  haWsNote = "connecting to " + host + ":" + String(port);
  haWs.begin(host.c_str(), port, "/api/websocket");
  haWs.onEvent(haWsOnEvent);
  haWs.setReconnectInterval(5000);
  // No WS-level heartbeat for now - HA has its own ping/pong and an
  // unexpected ping was a suspect for the early disconnects.
  haWsPhase = HAWS_CONNECTING;
}

// Called every loop() tick. Owns the WS lifecycle: starts/stops with the
// config toggle + WiFi, pumps the socket, and re-subscribes when the set
// of tracked entities changes.
void haWsLoop() {
  bool want = cfg.haLiveUpdates && haConfigured() && WiFi.status() == WL_CONNECTED;

  if (!want) { haWsStop(); return; }

  // Level-triggered (not edge): if we're meant to be live but idle, (re)start.
  // A settings save calls haWsStop() -> HAWS_OFF, and this picks it back up.
  if (haWsPhase == HAWS_OFF) haWsBegin();

  // FAILED (https URL / bad token) - retry every 30 s in case it was fixed
  // without a save.
  if (haWsPhase == HAWS_FAILED) {
    if (millis() - haWsSigCheckMs > 30000) { haWsSigCheckMs = millis(); haWsPhase = HAWS_OFF; }
    return;
  }
  if (haWsPhase == HAWS_OFF) return;

  haWs.loop();

  if (haWsPhase == HAWS_READY && millis() - haWsSigCheckMs > 2000) {
    haWsSigCheckMs = millis();
    if (haWsEntitySignature() != haWsEntitySig) {
      Serial.println("[haWs] entity set changed - reconnecting");
      haWs.disconnect();
      haWsPhase = HAWS_CONNECTING;
    }
  }
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

// Stateless "activate" tiles (scene / script / button): a filled play
// triangle, tinted with the accent while the post-tap flash is showing.
void drawActionIcon(TFT_eSprite& spr, int cx, int cy, uint16_t color) {
  spr.fillTriangle(cx - 6, cy - 8, cx - 6, cy + 8, cx + 8, cy, color);
}

// Compact ~16px corner icons for the redesigned tile layout: the big
// centred bulb/switch gave every tile the same "settings row" look, so
// the type now sits small in a corner and the value carries the tile.
// Compact ~16px corner icons. gMono -> outline-only 1px (state reads from
// the accent colour); otherwise filled when "on".
void drawMiniBulb(TFT_eSprite& spr, int cx, int cy, bool on, uint16_t color, uint16_t bg = COL_BG) {
  uint16_t c = on ? color : COL_DIM;
  if (on && !gMono) spr.fillSmoothCircle(cx, cy - 2, 6, scaleColor565(color, 50), bg);
  spr.drawSmoothCircle(cx, cy - 2, 6, c, on && !gMono ? scaleColor565(color, 50) : bg);
  spr.drawFastHLine(cx - 4, cy + 5, 8, c);
  spr.drawFastHLine(cx - 3, cy + 7, 6, c);
}
void drawMiniSwitch(TFT_eSprite& spr, int cx, int cy, bool on, uint16_t color, uint16_t bg = COL_BG) {
  uint16_t c = on ? color : COL_DIM;
  if (gMono) {
    spr.drawRect(cx - 11, cy - 6, 22, 12, c);
    spr.drawRect(on ? cx + 1 : cx - 11, cy - 6, 10, 12, c);
  } else {
    uint16_t track = on ? scaleColor565(color, 40) : bg;
    if (on) uiFillRR(spr, cx - 11, cy - 6, 22, 12, 6, track, bg);
    uiStrokeRR(spr, cx - 11, cy - 6, 22, 12, 6, c, on ? track : bg);
    spr.fillSmoothCircle(on ? cx + 5 : cx - 5, cy, 3, on ? fixColor565(TFT_WHITE) : COL_DIM, track);
  }
}
void drawMiniSensor(TFT_eSprite& spr, int cx, int cy, uint16_t color, uint16_t bg = COL_BG) {
  spr.drawSmoothCircle(cx, cy, 6, COL_DIM, bg);
  spr.drawWideLine(cx, cy, cx + 3, cy - 3, 1.3f, COL_DIM, bg);
  spr.fillSmoothCircle(cx, cy, 1, COL_DIM, bg);
}

// Tile label helper: dim, top-left, hard-cut; hairline rule under it in the
// mono theme.
void drawTileLabel(TFT_eSprite& spr, const String& text, int pad, int w, int maxWidth, uint16_t bg) {
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(COL_DIM, bg);
  uiDrawFitted(spr, text, pad, 9, maxWidth, 1, false);
  if (gTileRule) spr.drawFastHLine(pad, 27, w - pad * 2, COL_STROKE);
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
  // Anti-aliased corners (theme B) - blends the curve against the page bg.
  spr.fillSmoothRoundRect(0, 0, w, h, gCardRadius, fillColor, COL_BG);
  if (showTileBorder) uiStrokeRR(spr, 0, 0, w, h, gCardRadius, COL_STROKE, fillColor);
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
  const int pad = 12;

  drawTileLabel(sprTile, "Timer", pad, w, w - pad - 26, COL_PANEL);
  // On a 1x1 the clock icon crowds the mm:ss readout out of the tile -
  // only the wide layout has room for it.
  if (wide) drawClockIcon(sprTile, w - 22, h - 18, timerRunning ? COL_ACCENT : COL_DIM, 0.72f, COL_PANEL);

  sprTile.setTextColor(timerRunning ? COL_ACCENT : COL_TEXT, COL_PANEL);
  uiDrawFitted(sprTile, timeText, pad, h - 34, wide ? (w - pad - 24) : (w - pad * 2), 4);
  sprTile.setTextDatum(TL_DATUM);

  pushSpriteAndDelete(sprTile, x, y);
}

void drawWeatherTileSprite(int tileIdx, int x, int y, int w, int h, bool wide, bool force) {
  TileRuntime& rt = tileRuntime[currentPageIndex][tileIdx];

  String tempText = weatherKnown ? String((int)roundf(weatherCurrentTemp)) : "--";

  String combined = tempText + "|" + String(weatherCurrentCode) + "|" + String(weatherIsDay ? 1 : 0) + "|" + String(COL_PANEL) + "|" + String(COL_ACCENT);
  if (!force && combined == rt.cacheKey) return;
  rt.cacheKey = combined;

  makeSpriteCard(sprTile, w, h);
  const int pad = 12;

  drawTileLabel(sprTile, weatherKnown ? weatherCodeLabel(weatherCurrentCode) : String("Weather"),
                pad, w, w - pad - 30, COL_PANEL);

  if (weatherKnown) {
    drawWeatherIcon(sprTile, w - 25, h - 20, weatherCurrentCode, 0.72f, weatherIsDay, COL_PANEL);
  }

  sprTile.setTextColor(COL_TEXT, COL_PANEL);
  int ty = h - 34;
  uiDrawFitted(sprTile, tempText, pad, ty, w - pad * 2 - 12, 4);
  if (weatherKnown) {
    int nw = uiTextWidth(sprTile, tempText, 4);
    sprTile.drawSmoothCircle(pad + nw + 6, ty + 5, 3, COL_TEXT, COL_PANEL); // degree mark, drawn (cleaner than the glyph at this size)
  }
  sprTile.setTextDatum(TL_DATUM);

  pushSpriteAndDelete(sprTile, x, y);
}

// Small sun-over-horizon glyph with a direction arrow. cy is the horizon.
void drawSunHorizonIcon(TFT_eSprite& spr, int cx, int cy, bool rising, uint16_t lineColor, uint16_t bg = COL_BG) {
  uint16_t sun = fixColor565(0xFDE0);
  int r = 5;
  int scy = cy - 4;
  spr.fillSmoothCircle(cx, scy, r, sun, bg);
  spr.drawFastVLine(cx, scy - r - 3, 2, sun);
  spr.drawWideLine(cx - 6, scy - 6, cx - 4, scy - 4, 1.3f, sun, bg);
  spr.drawWideLine(cx + 6, scy - 6, cx + 4, scy - 4, 1.3f, sun, bg);
  spr.drawFastHLine(cx - 13, cy + 5, 26, lineColor);
  int ax = cx - 14, ay = scy;
  if (rising) spr.fillTriangle(ax - 3, ay + 3, ax + 3, ay + 3, ax, ay - 3, lineColor);
  else        spr.fillTriangle(ax - 3, ay - 3, ax + 3, ay - 3, ax, ay + 3, lineColor);
}

// mode: 0 = sunrise, 1 = sunset, 2 = both (for a 1x2 wide tile).
void drawSunTileSprite(int tileIdx, int x, int y, int w, int h, bool wide, bool force, int mode) {
  TileRuntime& rt = tileRuntime[currentPageIndex][tileIdx];

  String riseT = weatherKnown ? formatClockMin(weatherSunriseMin) : "...";
  String setT  = weatherKnown ? formatClockMin(weatherSunsetMin)  : "...";

  String combined = "SUN" + String(mode) + "|" + riseT + "|" + setT + "|" + String(COL_PANEL);
  if (!force && combined == rt.cacheKey) return;
  rt.cacheKey = combined;

  makeSpriteCard(sprTile, w, h);
  sprTile.setTextDatum(TC_DATUM);

  if (mode == 2) {
    int c1 = w / 4, c2 = w - w / 4;
    sprTile.drawFastVLine(w / 2, 14, h - 28, COL_STROKE);
    sprTile.setTextColor(COL_DIM, COL_PANEL);
    uiDrawString(sprTile, "Sunrise", c1, 9, 1);
    uiDrawString(sprTile, "Sunset",  c2, 9, 1);
    drawSunHorizonIcon(sprTile, c1, h / 2 + 2, true,  COL_DIM, COL_PANEL);
    drawSunHorizonIcon(sprTile, c2, h / 2 + 2, false, COL_DIM, COL_PANEL);
    sprTile.setTextColor(COL_TEXT, COL_PANEL);
    uiDrawFitted(sprTile, riseT, c1, h - 24, w / 2 - 20, fontTile());
    uiDrawFitted(sprTile, setT,  c2, h - 24, w / 2 - 20, fontTile());
  } else {
    bool rise = (mode == 0);
    const int pad = 12;
    drawTileLabel(sprTile, rise ? String("Sunrise") : String("Sunset"), pad, w, w - pad - 24, COL_PANEL);
    drawSunHorizonIcon(sprTile, w - 21, h - 18, rise, COL_DIM, COL_PANEL);
    sprTile.setTextColor(COL_TEXT, COL_PANEL);
    uiDrawFitted(sprTile, rise ? riseT : setT, pad, h - 27, w - pad * 2, 2);
  }

  sprTile.setTextDatum(TL_DATUM);
  pushSpriteAndDelete(sprTile, x, y);
}

// Numeric date, no leading zeros, matching the format the user picked per
// tile: false = M/D/YYYY (US, default), true = D/M/YYYY.
String formatDateNumeric(const struct tm& t, bool euro) {
  char b[16];
  int mo = t.tm_mon + 1, d = t.tm_mday, y = t.tm_year + 1900;
  if (euro) snprintf(b, sizeof(b), "%d/%d/%d", d, mo, y);
  else      snprintf(b, sizeof(b), "%d/%d/%d", mo, d, y);
  return String(b);
}

// Small tear-off-calendar glyph: rounded page, header rule, two binding tabs.
template<typename T>
void drawCalendarIcon(T& d, int cx, int cy, uint16_t c, uint16_t bg) {
  const int cw = 16, chh = 15;
  int x0 = cx - cw / 2, y0 = cy - chh / 2 + 1;
  uiStrokeRR(d, x0, y0, cw, chh, 3, c, bg);
  d.drawFastHLine(x0 + 2, y0 + 4, cw - 4, c);       // header rule under the "month" strip
  d.drawFastVLine(x0 + 4, y0 - 2, 3, c);            // binding tabs poking above the top edge
  d.drawFastVLine(x0 + cw - 5, y0 - 2, 3, c);
}

// mode 0 = compact numeric ("8/30/2026"); mode 1 = weekday-focused for a
// wide tile ("Sunday" big, date small underneath).
void drawDateTileSprite(int tileIdx, int x, int y, int w, int h, bool wide, bool force, bool euro, int mode) {
  TileRuntime& rt = tileRuntime[currentPageIndex][tileIdx];

  time_t now = time(nullptr);
  bool known = (now > 1700000000); // NTP has synced
  struct tm t = {};
  if (known) localtime_r(&now, &t);

  String dateStr = known ? formatDateNumeric(t, euro) : "--/--/----";
  String dayStr = "...";
  if (known) { char db[16]; strftime(db, sizeof(db), "%A", &t); dayStr = String(db); }

  String combined = "DATE" + String(mode) + "|" + dayStr + "|" + dateStr + "|" + String(COL_PANEL);
  if (!force && combined == rt.cacheKey) return;
  rt.cacheKey = combined;

  makeSpriteCard(sprTile, w, h);
  const int pad = 10;

  drawTileLabel(sprTile, "Date", pad, w, w - pad - 26, COL_PANEL);

  if (mode == 1 && wide) {
    // Weekday-focused: only the wide layout has room for the day name plus
    // the calendar glyph without anything running into the border.
    drawCalendarIcon(sprTile, w - 22, h - 18, COL_DIM, COL_PANEL);
    sprTile.setTextColor(COL_TEXT, COL_PANEL);
    uiDrawFitted(sprTile, dayStr, pad, h - 42, w - pad * 2 - 20, 4);
    sprTile.setTextColor(COL_DIM, COL_PANEL);
    uiDrawFitted(sprTile, dateStr, pad, h - 18, w - pad * 2, 2);
  } else {
    // Compact numeric, centred and full-width - the string is too wide to
    // sit beside an icon in a 1x1 cell, so there's no icon here.
    sprTile.setTextDatum(TC_DATUM);
    sprTile.setTextColor(COL_TEXT, COL_PANEL);
    uiDrawFitted(sprTile, dateStr, w / 2, h - 32, w - 12, 2);
  }

  sprTile.setTextDatum(TL_DATUM);
  pushSpriteAndDelete(sprTile, x, y);
}

// Post-tap "Sent" flash for a scene/script/button tile. -1 page = none.
int actionFlashPage = -1;
int actionFlashTile = -1;
unsigned long actionFlashStartMs = 0;
const unsigned long ACTION_FLASH_MS = 550;

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
  if (strcmp(tl.type, "sunrise") == 0) { drawSunTileSprite(tileIdx, x, y, w, h, wide, force, 0); return; }
  if (strcmp(tl.type, "sunset") == 0)  { drawSunTileSprite(tileIdx, x, y, w, h, wide, force, 1); return; }
  if (strcmp(tl.type, "sun") == 0)     { drawSunTileSprite(tileIdx, x, y, w, h, wide, force, 2); return; }
  if (strcmp(tl.type, "date") == 0)     { drawDateTileSprite(tileIdx, x, y, w, h, wide, force, tl.dateEuro, 0); return; }
  if (strcmp(tl.type, "datewide") == 0) { drawDateTileSprite(tileIdx, x, y, w, h, wide, force, tl.dateEuro, 1); return; }

  bool isAction = (strcmp(tl.type, "scene") == 0 || strcmp(tl.type, "script") == 0 ||
                   strcmp(tl.type, "button") == 0);
  if (isAction) {
    bool flashing = (actionFlashPage == currentPageIndex && actionFlashTile == tileIdx);
    String combined = String("ACT|") + tl.label + "|" + (flashing ? "1" : "0") + "|" + String(COL_PANEL);
    if (!force && combined == rt.cacheKey) return;
    rt.cacheKey = combined;

    uint16_t bg = flashing ? COL_PANEL_LIT : COL_PANEL;
    makeSpriteCard(sprTile, w, h, flashing ? (int)COL_PANEL_LIT : -1);

    sprTile.setTextDatum(TC_DATUM);
    sprTile.setTextColor(COL_DIM, bg);
    uiDrawFitted(sprTile, tl.label, w / 2, 4, w - 12, fontTile());

    drawActionIcon(sprTile, wide ? w / 4 : w / 2, 38, flashing ? COL_ACCENT : COL_DIM);

    sprTile.setTextDatum(MC_DATUM);
    sprTile.setTextColor(flashing ? COL_TEXT : COL_DIM, bg);
    uiDrawString(sprTile, flashing ? "Sent" : (strlen(tl.entityId) == 0 ? "Unset" : "Tap"), w / 2, h - 12, fontTile());
    sprTile.setTextDatum(TL_DATUM);

    pushSpriteAndDelete(sprTile, x, y);
    return;
  }

  bool isLight = (strcmp(tl.type, "light") == 0);
  bool isSwitch = (strcmp(tl.type, "switch") == 0);
  bool isSensor = (strcmp(tl.type, "sensor") == 0);

  bool unavail = rt.known && rt.unavailable; // entity offline (e.g. bulb killed at the wall switch)

  String stateText;
  if (strlen(tl.entityId) == 0) {
    stateText = "Unset";
  } else if (!haConfigured()) {
    stateText = "No HA";
  } else if (!rt.known) {
    stateText = (isLight || isSwitch) ? "Off" : "..."; // optimistic default; corrects on poll
  } else if (unavail) {
    stateText = "N/A";
  } else if (isSensor) {
    stateText = rt.lastRawState;
  } else if (isLight) {
    stateText = rt.on ? (String(rt.brightnessPct) + "%") : "Off";
  } else {
    stateText = rt.on ? "On" : "Off";
  }

  String combined = String(tl.label) + "|" + stateText + "|" + String(rt.on ? 1 : 0) + "|" +
                     String(unavail ? 1 : 0) + "|" + String(COL_PANEL) + "|" + String(COL_ACCENT);
  if (!force && combined == rt.cacheKey) return;
  rt.cacheKey = combined;

  // Lit-up background for a light that's on.
  bool useLitBg = isLight && rt.on && !unavail;
  uint16_t bgColor = useLitBg ? COL_PANEL_LIT : COL_PANEL;
  makeSpriteCard(sprTile, w, h, useLitBg ? (int)COL_PANEL_LIT : -1);
  if (useLitBg) uiStrokeRR(sprTile, 0, 0, w, h, gCardRadius, COL_ACCENT, bgColor); // accent rim when on

  const int pad = 12;
  bool activeState = ((isLight || isSwitch) && rt.on && !unavail);

  drawTileLabel(sprTile, tl.label, pad, w, w - pad - 6, bgColor);

  // --- Value: bottom-left, the tile's focal point ---
  bool shortVal = (stateText.length() <= 4);
  int vFont = shortVal ? (cfg.uiFontSize == 0 ? 2 : 4) : 2;
  uint16_t valColor = unavail ? COL_DIM
                    : activeState ? COL_ACCENT
                    : (rt.known || isSensor || strlen(tl.entityId) == 0) ? COL_TEXT : COL_DIM;
  sprTile.setTextColor(valColor, bgColor);
  int vy = h - (vFont == 4 ? 36 : 27);
  uiDrawFitted(sprTile, stateText, pad, vy, w - pad - 26, vFont);
  sprTile.setTextDatum(TL_DATUM);

  // --- Type icon: bottom-right, out of the label's way ---
  int mIconX = w - 19;
  int mIconY = h - 19;
  if (isLight)       drawMiniBulb(sprTile, mIconX, mIconY, rt.on && !unavail, COL_ACCENT, bgColor);
  else if (isSwitch) drawMiniSwitch(sprTile, mIconX, mIconY, rt.on && !unavail, COL_ACCENT, bgColor);
  else               drawMiniSensor(sprTile, mIconX, mIconY, COL_DIM, bgColor);

  // Diagonal strike-through when the entity is unavailable - reads as
  // "crossed out / not reachable" regardless of tile type.
  if (unavail) {
    sprTile.drawWideLine(mIconX - 10, mIconY - 10, mIconX + 10, mIconY + 10, 2.0f, COL_TEXT, bgColor);
  }

  pushSpriteAndDelete(sprTile, x, y);
}

// =========================================================
// BRIGHTNESS OVERLAY - the long-press-to-dim slider
// =========================================================
// The old slider lived inside the tile: ~44 px of travel for the whole
// 0-100% range, which the resistive panel's jitter made frustrating to
// land. Long-press now snaps up a large centred slider that owns the
// screen until the finger lifts (~135 px of travel, ~0.7%/px), then the
// page redraws. Drawn straight onto the TFT (no sprite) so it can sit on
// top of the grid without disturbing it; only the value + fill repaint
// during a drag.
const int BRO_W = 182, BRO_H = 234;
const int BRO_X = (SCREEN_W - BRO_W) / 2;
const int BRO_Y = HEADER_H + 8;
const int BRO_VAL_Y  = BRO_Y + 38;
const int BRO_TRK_W  = 66;
const int BRO_TRK_X  = SCREEN_W / 2 - BRO_TRK_W / 2;
const int BRO_TRK_Y  = BRO_Y + 82;
const int BRO_TRK_H  = BRO_Y + BRO_H - 18 - BRO_TRK_Y;

void drawBrightnessOverlayValue(int percent) {
  // Percentage readout - the clear box is deliberately generous since the
  // big font is tall, so no glyph fragments survive between updates.
  tft.fillRect(BRO_X + 6, BRO_VAL_Y - 4, BRO_W - 12, 42, COL_PANEL);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_ACCENT, COL_PANEL);
  uiDrawString(tft, String(percent) + "%", SCREEN_W / 2, BRO_VAL_Y, 4);
  tft.setTextDatum(TL_DATUM);

  // Track interior: full clear, then a flat fill rising from the bottom.
  // Plain fillRect only - no anti-aliased edges and no handle overhanging
  // the track - so nothing is left behind as the level sweeps up and down.
  int ix = BRO_TRK_X + 4, iw = BRO_TRK_W - 8;
  int iy = BRO_TRK_Y + 4, ih = BRO_TRK_H - 8;
  tft.fillRect(ix, iy, iw, ih, COL_PANEL);
  int fillH = ih * percent / 100;
  if (fillH > 0) tft.fillRect(ix, iy + ih - fillH, iw, fillH, COL_ACCENT);
}

void drawBrightnessOverlay(const String& label, int percent) {
  uiFillRR(tft, BRO_X, BRO_Y, BRO_W, BRO_H, 18, COL_PANEL, COL_BG);
  uiStrokeRR(tft, BRO_X, BRO_Y, BRO_W, BRO_H, 18, COL_ACCENT, COL_PANEL);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_DIM, COL_PANEL);
  uiDrawFitted(tft, label, SCREEN_W / 2, BRO_Y + 12, BRO_W - 24, 2);

  uiStrokeRR(tft, BRO_TRK_X, BRO_TRK_Y, BRO_TRK_W, BRO_TRK_H, 12, COL_STROKE, COL_PANEL);
  drawBrightnessOverlayValue(percent);
  tft.setTextDatum(TL_DATUM);
}

// Drag is relative to where the finger was when the slider engaged: moving
// the full track height up or down covers the whole 0-100% range, so the
// value starts exactly at the light's current level and never jumps.
int brightnessOverlayPctFromDrag(int y) {
  int range = max(1, BRO_TRK_H - 8);
  return constrain(sliderAnchorPct + ((sliderAnchorY - y) * 100) / range, 0, 100);
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

  // 12-hour text ("11:32 PM") needs a bit more room than 24-hour ("23:32"),
  // and font 4 needs more room than font 2 for the same text. Computed up
  // front so the title can be width-fitted into the space that's left.
  int clockAreaW;
  if (hFont == 4) {
    clockAreaW = cfg.use12Hour ? 132 : 84;
  } else {
    clockAreaW = cfg.use12Hour ? 100 : 62;
  }

  if (force) {
    tft.fillRect(0, 0, SCREEN_W, HEADER_H, COL_BG);
    tft.drawFastHLine(0, HEADER_H - 1, SCREEN_W, COL_STROKE);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_TEXT, COL_BG);
    uiDrawFitted(tft, pg.name, 8, titleY, SCREEN_W - clockAreaW - 12, hFont);
    lastHeaderTimeText = "";
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

// Footer slot for a page. Slots are divided among the VISIBLE pages only;
// a hidden page gets a zero-width off-screen rect so it's never hit-tested
// or drawn.
void getFooterDotRect(int pageIdx, int& x, int& y, int& w, int& h) {
  int count = visiblePageCount();
  int vp = visiblePosOf(pageIdx);
  y = SCREEN_H - FOOTER_H;
  h = FOOTER_H;
  if (vp < 0 || count <= 0) { x = -100; w = 0; return; }
  int spacing = SCREEN_W / count;
  x = vp * spacing;
  w = spacing;
}

void drawFooter(bool force) {
  if (!force) return; // footer only changes on page switch, always full-redraw

  tft.fillRect(0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H, COL_BG);
  tft.drawFastHLine(0, SCREEN_H - FOOTER_H, SCREEN_W, COL_STROKE);

  // Per-page-type icon per visible nav slot (bulb / cloud / clock / gear):
  // accent for the current page, dim otherwise.
  for (int i = 0; i < cfg.pageCount; i++) {
    if (cfg.pages[i].hidden) continue;
    int x, y, w, h;
    getFooterDotRect(i, x, y, w, h);
    drawPageTypeIcon(tft, cfg.pages[i].type, x + w / 2, y + h / 2,
                     i == currentPageIndex ? COL_ACCENT : COL_DIM, 0.6f);
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

  // Soft drop shadow under each card (light mode only - it needs a lighter
  // ground to read against). Drawn now; the tile sprite pushes over the
  // card itself and leaves the offset edge peeking out bottom-right.
  bool shadow = !showTileBorder;
  uint16_t shadowCol = fixColor565(0xCE9A); // #ccd0d4, ~1 step darker than the light bg

  for (int i = 0; i < pg.tileCount; i++) {
    if (slotOf[i] < 0) continue;
    bool wide = (pg.tiles[i].size == 2);
    int x, y, w, h;
    getSlotRect(slotOf[i], wide, x, y, w, h);
    if (shadow) tft.fillSmoothRoundRect(x + 2, y + 3, w, h, gCardRadius, shadowCol, COL_BG);
    tft.fillSmoothRoundRect(x, y, w, h, gCardRadius, COL_PANEL, COL_BG);
    if (showTileBorder) uiStrokeRR(tft, x, y, w, h, gCardRadius, COL_STROKE, COL_PANEL);
  }
}

// Shared between drawing and touch hit-testing so the two can never drift
// out of sync with each other.
// 4 info rows (WiFi / IP / Host / HA), then Theme + Flip side by side,
// then a full-width Reboot button.
const int STATUS_INFO_Y0  = HEADER_H + 20;                     // 54
const int STATUS_ROW_H    = 27;
const int STATUS_HA_ROW_Y = STATUS_INFO_Y0 + 3 * STATUS_ROW_H; // 135
const int STATUS_BTN_H     = 34;
const int STATUS_BTN_FULL_X = 12, STATUS_BTN_FULL_W = 216;
const int STATUS_BTN_L_X   = 12,  STATUS_BTN_HALF_W = 103;
const int STATUS_BTN_R_X   = 125;
const int STATUS_ROW1_BTN_Y = STATUS_INFO_Y0 + 4 * STATUS_ROW_H + 18; // 180  (Theme | Flip)
const int STATUS_ROW2_BTN_Y = STATUS_ROW1_BTN_Y + STATUS_BTN_H + 12;  // 226  (Reboot); bottom 260

void drawStatusButton(int bx, int by, int bw, const String& label, bool danger) {
  uint16_t fill = danger ? fixColor565(0xF36D) : COL_PANEL;
  uint16_t txt  = danger ? fixColor565(0xFFFF) : COL_TEXT;
  uiFillRR(tft, bx, by, bw, STATUS_BTN_H, gCardRadius, fill);
  if (showTileBorder) uiStrokeRR(tft, bx, by, bw, STATUS_BTN_H, gCardRadius, COL_STROKE, fill);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(txt, fill);
  uiDrawFitted(tft, label, bx + bw / 2, by + STATUS_BTN_H / 2, bw - 14, 2, false);
  tft.setTextDatum(TL_DATUM);
}

void drawStatusRow(int idx, const char* key, const String& val, uint16_t valColor) {
  int ry = STATUS_INFO_Y0 + idx * STATUS_ROW_H;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  uiDrawString(tft, key, 14, ry, 2);
  tft.setTextColor(valColor, COL_BG);
  uiDrawFitted(tft, val, 86, ry, SCREEN_W - 86 - 14, 2, false);
}

void drawStatusPageFull() {
  tft.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H - FOOTER_H, COL_BG);

  bool wifi = (WiFi.status() == WL_CONNECTED);

  drawStatusRow(0, "WiFi", wifi ? (String(WiFi.RSSI()) + " dBm") : String("Not connected"), COL_TEXT);
  {
    String ipText = wifi ? WiFi.localIP().toString() : String("-");
    if (staticIpFellBack) ipText += "  !DHCP";
    drawStatusRow(1, "IP", ipText, COL_TEXT);
  }
  drawStatusRow(2, "Host", String(cfg.deviceName) + ".local", COL_TEXT);

  uint16_t haColor = (haConnState == HA_CONN_OK) ? fixColor565(0x5E91)          // green
                   : (haConnState == HA_CONN_AUTH_FAIL ||
                      haConnState == HA_CONN_UNREACHABLE) ? fixColor565(0xF36D) // red
                   : COL_DIM;
  drawStatusRow(3, "HA", haConnLabel(haConnState), haColor);

  drawStatusButton(STATUS_BTN_L_X, STATUS_ROW1_BTN_Y, STATUS_BTN_HALF_W,
                   cfg.darkTheme ? "Dark" : "Light", false);
  drawStatusButton(STATUS_BTN_R_X, STATUS_ROW1_BTN_Y, STATUS_BTN_HALF_W,
                   cfg.flipScreen ? "Flip: On" : "Flip: Off", false);
  drawStatusButton(STATUS_BTN_FULL_X, STATUS_ROW2_BTN_Y, STATUS_BTN_FULL_W,
                   rebootArmed ? "Tap again to reboot" : "Reboot", rebootArmed);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  uiDrawString(tft, String(FW_NAME) + "  v" + FW_VERSION,
               SCREEN_W / 2, STATUS_ROW2_BTN_Y + STATUS_BTN_H + 3, 1);
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
  // own weather card: icon + condition + weather location on the left,
  // current temp + today's high/low on the right.
  const int heroTop = HEADER_H;
  const int heroIconCx = 40;
  const int heroIconCy = heroTop + 38;

  drawWeatherIcon(tft, heroIconCx, heroIconCy, weatherCurrentCode, 1.5f, weatherIsDay, COL_BG);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  uiDrawString(tft, weatherCodeLabel(weatherCurrentCode), 68, heroTop + 12, fontHeader());
  tft.setTextColor(COL_DIM, COL_BG);
  uiDrawFitted(tft, String(cfg.weatherLocationName), 68, heroTop + 40, SCREEN_W - 68 - 58, fontStatusRow());

  String curTemp = String((int)roundf(weatherCurrentTemp));
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  int curTempW = tft.textWidth(curTemp, 4);
  uiDrawString(tft, curTemp, SCREEN_W - 8, heroTop + 10, 4);
  tft.drawSmoothCircle(SCREEN_W - 8 - curTempW - 6, heroTop + 14, 2, COL_TEXT, COL_BG);

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
    tft.drawSmoothCircle(cx + hiW / 2 + 4, bodyTop + 76, 2, COL_TEXT, COL_BG);

    tft.setTextColor(COL_DIM, COL_BG);
    String lo = String((int)roundf(forecast[i].tempMin));
    int loW = tft.textWidth(lo, rf);
    uiDrawString(tft, lo, cx, bodyTop + 106, rf);
    tft.drawSmoothCircle(cx + loW / 2 + 4, bodyTop + 100, 2, COL_DIM, COL_BG);
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

    uiFillRR(tft, TIMER_CANCEL_BTN_X, TIMER_CANCEL_BTN_Y, TIMER_CANCEL_BTN_W, TIMER_CANCEL_BTN_H, gCardRadius, COL_PANEL);
    if (showTileBorder) uiStrokeRR(tft, TIMER_CANCEL_BTN_X, TIMER_CANCEL_BTN_Y, TIMER_CANCEL_BTN_W, TIMER_CANCEL_BTN_H, gCardRadius, COL_STROKE, COL_PANEL);
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
    uiFillRR(tft, x, y, w, h, gCardRadius, COL_PANEL);
    if (showTileBorder) uiStrokeRR(tft, x, y, w, h, gCardRadius, COL_STROKE, COL_PANEL);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_TEXT, COL_PANEL);
    String label = formatPresetLabel(cfg.timerPresetSec[i]);
    uiDrawString(tft, label, x + w / 2, y + h / 2, fontTile());
  }

  int cx, cy, cw, ch;
  getSlotRect(5, false, cx, cy, cw, ch);
  uiFillRR(tft, cx, cy, cw, ch, gCardRadius, COL_PANEL);
  if (showTileBorder) uiStrokeRR(tft, cx, cy, cw, ch, gCardRadius, COL_STROKE, COL_PANEL);
  // "Flash Lights" won't fit on one line in the wider mono typeface, so
  // it's stacked as two words with a small toggle underneath. Everything
  // is placed with a margin off every edge - the tile is only 70 px tall.
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_DIM, COL_PANEL);
  uiDrawFitted(tft, "Flash", cx + cw / 2, cy + 5, cw - 8, 1, false);
  uiDrawFitted(tft, "Lights", cx + cw / 2, cy + 25, cw - 8, 1, false);
  tft.setTextDatum(TL_DATUM);

  const int pw = 34, ph = 14;
  int px = cx + cw / 2 - pw / 2;
  int py = cy + ch - ph - 7;
  bool on = cfg.flashOnExpire;
  uint16_t track = on ? COL_ACCENT : COL_PANEL_ALT;
  uiFillRR(tft, px, py, pw, ph, ph / 2, track, COL_PANEL);
  uiStrokeRR(tft, px, py, pw, ph, ph / 2, on ? COL_ACCENT : COL_STROKE, track);
  int kr = ph / 2 - 3;
  int kx = on ? (px + pw - ph / 2) : (px + ph / 2);
  uiFillCircle(tft, kx, py + ph / 2, kr, fixColor565(TFT_WHITE));
}

void updateGridTiles(bool force) {
  // The brightness overlay owns the screen while it's up - repainting tiles
  // underneath it would bleed through.
  if (sliderOverlayShown) return;

  PageConfig& pg = cfg.pages[currentPageIndex];
  int slotOf[MAX_TILES];
  layoutPageTiles(pg, slotOf);

  for (int i = 0; i < pg.tileCount; i++) {
    bool wide = (pg.tiles[i].size == 2);
    drawTileSprite(i, slotOf[i], wide, force);
  }
}

// Full-screen "join my Wi-Fi to set me up" card, shown while gProvisioning.
void drawProvisioningScreen(bool retrying) {
  tft.fillScreen(COL_BG);
  int cx = SCREEN_W / 2;

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  uiDrawString(tft, "WI-FI SETUP", cx, 40, 1);

  tft.setTextColor(COL_TEXT, COL_BG);
  uiDrawFitted(tft, "Join this network", cx, 78, SCREEN_W - 24, 2, false);

  uiFillRR(tft, 16, 108, SCREEN_W - 32, 52, gCardRadius, COL_PANEL, COL_BG);
  if (showTileBorder) uiStrokeRR(tft, 16, 108, SCREEN_W - 32, 52, gCardRadius, COL_ACCENT, COL_PANEL);
  tft.setTextColor(COL_ACCENT, COL_PANEL);
  uiDrawFitted(tft, gApSsid, cx, 122, SCREEN_W - 44, 4, true);

  tft.setTextColor(COL_DIM, COL_BG);
  uiDrawFitted(tft, "then follow the page", cx, 178, SCREEN_W - 24, 2, false);
  uiDrawFitted(tft, "that pops up", cx, 200, SCREEN_W - 24, 2, false);
  uiDrawFitted(tft, "(or open 192.168.4.1)", cx, 228, SCREEN_W - 24, 1, false);

  if (retrying) {
    tft.setTextColor(COL_DIM, COL_BG);
    uiDrawFitted(tft, "retrying saved network...", cx, 264, SCREEN_W - 24, 1, false);
  }
  tft.setTextDatum(TL_DATUM);
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

  // Clear an expired scene/script/button "Sent" flash and redraw that tile.
  if (actionFlashPage >= 0 && millis() - actionFlashStartMs > ACTION_FLASH_MS) {
    int fp = actionFlashPage, ft = actionFlashTile;
    actionFlashPage = -1;
    actionFlashTile = -1;
    if (fp == currentPageIndex && ft < pg.tileCount) {
      tileRuntime[fp][ft].cacheKey = "";
      int slotOf[MAX_TILES];
      layoutPageTiles(pg, slotOf);
      drawTileSprite(ft, slotOf[ft], pg.tiles[ft].size == 2, true);
    }
  }

  if (strcmp(pg.type, "home") == 0 || strcmp(pg.type, "area") == 0) {
    updateGridTiles(false);
  } else if (strcmp(pg.type, "timers") == 0) {
    updateTimersCountdownText();
  }
}

// =========================================================
// POLLING - the fallback when live updates are off (or the socket is down).
// loop() calls this on page-open and on a timer; the interval stretches
// right out while the WebSocket is carrying updates.
// =========================================================
void pollCurrentPageTiles() {
  PageConfig& pg = cfg.pages[currentPageIndex];
  if (strcmp(pg.type, "home") != 0 && strcmp(pg.type, "area") != 0) return;
  if (!haConfigured() || WiFi.status() != WL_CONNECTED) return;
  if (sliderOverlayShown) return; // each fetch blocks - would stutter the drag

  for (int i = 0; i < pg.tileCount; i++) {
    TileConfig& tl = pg.tiles[i];
    if (strlen(tl.entityId) == 0) continue;
    if (strcmp(tl.type, "light") != 0 && strcmp(tl.type, "switch") != 0 && strcmp(tl.type, "sensor") != 0) continue;

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
      if (cfg.pages[i].hidden) return; // page is hidden from nav - do nothing
      currentPageIndex = i;
      pageDirty = true;
      return;
    }
  }
}

void handleTileTap(int tileIdx) {
  PageConfig& pg = cfg.pages[currentPageIndex];
  TileConfig& tl = pg.tiles[tileIdx];

  // Weather / sun / Timer tiles aren't toggleable - tapping jumps to their page.
  if (strcmp(tl.type, "weather") == 0 || strcmp(tl.type, "sunrise") == 0 ||
      strcmp(tl.type, "sunset") == 0 || strcmp(tl.type, "sun") == 0) {
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

  int slotOfTap[MAX_TILES];
  layoutPageTiles(pg, slotOfTap);

  // scene / script / button: fire the service, show a brief "Sent" flash.
  if (strcmp(tl.type, "scene") == 0 || strcmp(tl.type, "script") == 0 ||
      strcmp(tl.type, "button") == 0) {
    actionFlashPage = currentPageIndex;
    actionFlashTile = tileIdx;
    actionFlashStartMs = millis();
    rt.cacheKey = "";
    drawTileSprite(tileIdx, slotOfTap[tileIdx], tl.size == 2, true);
    queueHaActivate(tl.entityId);
    return;
  }

  bool newState = !(rt.known && rt.on);

  // Don't paint an optimistic on/brightness for an entity HA says is
  // unavailable (bulb has no power) - the command goes out anyway, but the
  // tile keeps showing N/A until a real state says otherwise.
  if (!(rt.known && rt.unavailable)) {
    rt.on = newState;
    rt.known = true;
    rt.cacheKey = "";
    drawTileSprite(tileIdx, slotOfTap[tileIdx], tl.size == 2, true);
  }

  queueHaOnOff(tl.entityId, newState);
}

void handleStatusPageTap(int x, int y) {
  // HA row - tap anywhere on it to re-probe the connection now.
  if (y >= STATUS_HA_ROW_Y - 8 && y < STATUS_HA_ROW_Y + STATUS_ROW_H - 4) {
    haConnState = HA_CONN_UNKNOWN;
    lastHaCheckMs = 0;
    pageDirty = true;
    return;
  }

  // Row 1: Theme (left half) | Flip (right half)
  if (y >= STATUS_ROW1_BTN_Y && y < STATUS_ROW1_BTN_Y + STATUS_BTN_H) {
    if (x >= STATUS_BTN_L_X && x < STATUS_BTN_L_X + STATUS_BTN_HALF_W) {
      cfg.darkTheme = !cfg.darkTheme;
      applyTheme(cfg.darkTheme);
      saveConfig();
      pageDirty = true;
      return;
    }
    if (x >= STATUS_BTN_R_X && x < STATUS_BTN_R_X + STATUS_BTN_HALF_W) {
      cfg.flipScreen = !cfg.flipScreen;
      applyScreenRotation();
      saveConfig();
      pageDirty = true;
      return;
    }
  }

  // Row 2: Reboot (full width)
  if (x >= STATUS_BTN_FULL_X && x < STATUS_BTN_FULL_X + STATUS_BTN_FULL_W &&
      y >= STATUS_ROW2_BTN_Y && y < STATUS_ROW2_BTN_Y + STATUS_BTN_H) {
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
    cfg.flashOnExpire = !cfg.flashOnExpire;
    saveConfig(); // remember the choice across reboots
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
  uiFillRR(tft, DIALOG_X, DIALOG_Y, DIALOG_W, DIALOG_H, gCardRadius, COL_PANEL);
  if (showTileBorder) uiStrokeRR(tft, DIALOG_X, DIALOG_Y, DIALOG_W, DIALOG_H, gCardRadius, COL_STROKE, COL_PANEL);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_TEXT, COL_PANEL);
  uiDrawString(tft, "Timer Expired", DIALOG_X + DIALOG_W / 2, DIALOG_Y + 26, 2);

  uiFillRR(tft, DIALOG_STOP_X, DIALOG_BTN_Y, DIALOG_BTN_W, DIALOG_BTN_H, 8, COL_PANEL_ALT, COL_PANEL);
  uiStrokeRR(tft, DIALOG_STOP_X, DIALOG_BTN_Y, DIALOG_BTN_W, DIALOG_BTN_H, 8, COL_STROKE, COL_PANEL_ALT);
  tft.setTextColor(COL_TEXT, COL_PANEL_ALT);
  uiDrawString(tft, "Stop", DIALOG_STOP_X + DIALOG_BTN_W / 2, DIALOG_BTN_Y + DIALOG_BTN_H / 2, 2);

  uiFillRR(tft, DIALOG_RESTART_X, DIALOG_BTN_Y, DIALOG_BTN_W, DIALOG_BTN_H, 8, COL_ACCENT, COL_PANEL);
  uiStrokeRR(tft, DIALOG_RESTART_X, DIALOG_BTN_Y, DIALOG_BTN_W, DIALOG_BTN_H, 8, COL_ACCENT, COL_ACCENT);
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

  // --- Active brightness slider: owns the whole touch stream. Nothing here
  // makes a blocking network call (that froze the drag) - the value is only
  // pushed to HA once, on release. Resistive-contact flicker is filtered so
  // a moving finger isn't misread as a release, and a stray blip after the
  // real release can't hold the overlay open. ---
  if (sliderActive) {
    static unsigned long lostSinceMs = 0; // first moment contact went away (0 = solid)
    static unsigned long downSinceMs = 0; // first moment of the current contact run
    int idx = touchTileIndex;
    bool valid = (idx >= 0 && idx < pg.tileCount);
    if (!valid) down = false; // tile vanished under us - fall through to release

    if (down) {
      if (downSinceMs == 0) downSinceMs = millis();
      // After a dropout, only trust contact again once it's held ~40 ms -
      // isolated post-release blips never last that long.
      if (lostSinceMs != 0 && millis() - downSinceMs < 40) { wasDown = true; return; }
      lostSinceMs = 0;

      // Don't move the value until the finger has clearly travelled - that
      // way a still "long press" stays a tap. Re-anchor at that point so
      // the value doesn't jump when the drag starts.
      if (!sliderDragged && abs(y - sliderAnchorY) > 14) {
        sliderDragged = true;
        sliderAnchorY = y;
        sliderAnchorPct = sliderPercent;
      }
      if (sliderDragged) {
        int pct = brightnessOverlayPctFromDrag(y);
        if (pct != sliderPercent) {
          sliderPercent = pct;
          drawBrightnessOverlayValue(sliderPercent);
        }
      }
      wasDown = true;
      return;
    }

    // not down
    downSinceMs = 0;
    if (lostSinceMs == 0) lostSinceMs = millis();
    if (millis() - lostSinceMs < SLIDER_RELEASE_GRACE_MS) { wasDown = true; return; }

    // Sustained release. If the finger never actually moved, the "long
    // press" was really just a slow tap - dismiss the overlay and let it
    // toggle the tile instead of stealing the tap.
    lostSinceMs = 0;
    bool dragged = sliderDragged;
    int sendPct = sliderPercent;
    if (valid && dragged) {
      TileRuntime& rt = tileRuntime[currentPageIndex][idx];
      rt.brightnessPct = sendPct;
      rt.on = sendPct > 0;
      rt.known = true;
      rt.cacheKey = "";
    }
    sliderActive = false;
    sliderOverlayShown = false;
    sliderDragged = false;
    touchDown = false;
    touchMoved = false;
    swipeCandidate = false;
    wasDown = false;
    lastTouchReleaseMs = millis();

    drawCurrentPageFull(); // overlay gone immediately
    if (valid) {
      if (dragged) queueHaBrightness(pg.tiles[idx].entityId, sendPct);
      else         handleTileTap(idx); // still "long press" -> treat as a tap/toggle
    }
    touchTileIndex = -1;
    return;
  }

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
    // sliderActive is handled entirely by the dedicated block above, so
    // here we're only ever tracking a not-yet-classified press.
    int idx = touchTileIndex;
    TileConfig& tl = pg.tiles[idx];
    bool isLight = (strcmp(tl.type, "light") == 0);

    int dx = abs(x - touchStartX);
    int dy = abs(y - touchStartY);
    if (dx > MOVE_TOLERANCE || dy > MOVE_TOLERANCE) touchMoved = true;

    if (isLight && !touchMoved && (millis() - touchStartMs) >= LONG_PRESS_MS) {
      sliderActive = true;
      sliderDragged = false;
      TileRuntime& rt = tileRuntime[currentPageIndex][idx];
      sliderPercent = constrain(rt.brightnessPct, 0, 100);
      sliderAnchorY = y;
      sliderAnchorPct = sliderPercent;
      rt.cacheKey = "";

      drawBrightnessOverlay(tl.label, sliderPercent);
      sliderOverlayShown = true;
    }
  } else if (!down && wasDown) {
    lastTouchReleaseMs = millis();

    if (touchDown && touchTileIndex >= 0) {
      int idx = touchTileIndex;

      if (!touchMoved) handleTileTap(idx);

      touchDown = false;
      touchTileIndex = -1;
      sliderActive = false;
      touchMoved = false;
    } else if (swipeCandidate) {
      int dx = x - swipeStartX;
      int dy = abs(y - swipeStartY);

      if (abs(dx) >= SWIPE_THRESHOLD_X && dy < 40) {
        int target = (dx < 0) ? nextVisiblePage(currentPageIndex) : prevVisiblePage(currentPageIndex);
        if (target != currentPageIndex) { currentPageIndex = target; pageDirty = true; }
      } else if (swipeStartY >= SCREEN_H - FOOTER_H) {
        // Footer icon tap takes priority over any page-specific handling.
        for (int i = 0; i < cfg.pageCount; i++) {
          if (cfg.pages[i].hidden) continue;
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

// One-shot banner shown at the top of the settings page after a redirect
// (e.g. the result of the HA connection probe / config import on Save).
String gSaveNotice = "";

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
  h += "h1{font-size:22px;margin:4px 0 4px;letter-spacing:0.03em;}";
  h += "h1 .dim{color:var(--dim);font-weight:400;}";
  h += "h2{font-size:13px;margin:0 0 12px;color:var(--text);text-transform:uppercase;letter-spacing:0.08em;border-left:3px solid var(--accent);padding-left:10px;}";
  h += "h3{font-size:12px;margin:18px 0 6px;color:var(--dim);text-transform:uppercase;letter-spacing:0.07em;}";
  h += ".card{border:1px solid var(--border);border-radius:16px;padding:16px 16px 18px;margin:16px 0;}";
  h += "a{color:var(--accent);text-decoration:none;} a:hover{text-decoration:underline;}";
  h += "label{display:block;font-size:13px;color:var(--dim);margin:10px 0 4px;}";
  h += ".check{display:flex;align-items:center;gap:10px;margin:14px 0 6px;}";
  h += ".check label{margin:0;}";
  h += ".grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px;}";
  h += ".check input[type=radio]{width:18px;height:18px;flex:none;margin:0;}";
  // Checkboxes render as a coloured on/off toggle: grey track = off,
  // accent track = on.
  h += ".check input[type=checkbox]{appearance:none;-webkit-appearance:none;position:relative;width:44px;height:26px;flex:none;margin:0;padding:0;border-radius:999px;background:var(--panel2);border:1px solid var(--border);cursor:pointer;transition:background .15s,border-color .15s;}";
  h += ".check input[type=checkbox]::before{content:'';position:absolute;top:3px;left:3px;width:18px;height:18px;border-radius:50%;background:var(--dim);transition:transform .15s,background .15s;}";
  h += ".check input[type=checkbox]:checked{background:var(--accent);border-color:var(--accent);}";
  h += ".check input[type=checkbox]:checked::before{transform:translateX(18px);background:#0b1f28;}";
  h += ".check input[type=checkbox]:focus-visible{outline:2px solid var(--accent);outline-offset:2px;}";
  h += "input,select{padding:10px 12px;margin:0;width:100%;background:var(--panel2);color:var(--text);border:1px solid var(--border);border-radius:10px;font-size:14px;font-family:var(--font);}";
  h += "input:focus,select:focus{outline:none;border-color:var(--accent);}";
  h += "button{padding:9px 16px;margin:10px 6px 0 0;background:var(--panel2);color:var(--text);border:1px solid var(--border);border-radius:10px;font-size:13px;font-family:var(--font);cursor:pointer;}";
  h += "button.primary{background:var(--accent);color:#062230;border-color:var(--accent);font-weight:600;}";
  h += "button.danger{background:#3a1620;color:#ff8a9b;border-color:#5a2230;}";
  h += "button:disabled{opacity:0.35;cursor:default;}";
  h += "a.btnlink{display:inline-block;margin-top:4px;padding:9px 16px;background:var(--accent);color:#062230;border:1px solid var(--accent);border-radius:10px;font-size:13px;font-weight:600;}";
  h += "a.btnlink:hover{text-decoration:none;}";
  h += ".seg{display:flex;gap:4px;background:var(--panel2);border:1px solid var(--border);border-radius:999px;padding:4px;margin:2px 0 14px;}";
  h += ".seg button{flex:1;margin:0;padding:8px 0;border:none;background:transparent;color:var(--dim);border-radius:999px;font-weight:600;transition:background .15s,color .15s;}";
  h += ".seg button.seg-on{background:var(--accent);color:#0b1f28;}";
  h += ".row{background:var(--panel);border:1px solid var(--border);border-radius:14px;padding:14px 16px;margin:10px 0;}";
  h += ".row b{font-size:15px;}";
  h += ".muted{color:var(--dim);font-size:13px;}";
  h += "form{margin:0;}";
  h += ".notice{background:var(--panel2);border:1px solid var(--accent);border-radius:10px;padding:10px 12px;margin:12px 0;font-size:13px;}";
  h += ".pill{display:inline-block;padding:2px 9px;border-radius:999px;font-size:12px;border:1px solid var(--border);color:var(--dim);}";
  h += ".pill.ok{color:#7ee2b8;border-color:#2f6b52;}";
  h += ".pill.bad{color:#ff8a9b;border-color:#5a2230;}";
  h += ".pill.warn{color:#e2c97e;border-color:#6b5f2f;}";
  h += ".drag-handle{cursor:grab;display:inline-block;padding:0 10px 0 2px;margin-right:4px;color:var(--dim);font-size:17px;line-height:1;vertical-align:-2px;touch-action:none;user-select:none;}";
  h += ".drag-handle:active{cursor:grabbing;}";
  h += ".drag-float{position:fixed;z-index:999;margin:0!important;box-shadow:0 10px 28px rgba(0,0,0,.55);opacity:.97;transform:scale(1.03);user-select:none;}";
  h += ".drop-ph{border:2px dashed var(--accent);border-radius:14px;margin:10px 0;background:rgba(79,195,247,.10);}";
  h += "body.dragging{cursor:grabbing;user-select:none;}";
  h += "</style></head><body><div class='wrap'>";
  return h;
}

const String htmlFooter = "</div></body></html>";

// JS for the tile Add/Edit forms: when the Type changes, fetch that
// domain's entities from /ha/entities and fill the <datalist id='entlist'>.
// Fails soft - on any error the plain text field still accepts a typed ID.
String entityPickerScript() {
  return F(
    "<script>(function(){"
    "var sel=document.querySelector('select[name=type]');"
    "var dl=document.getElementById('entlist');"
    "var hint=document.getElementById('entHint');"
    "if(!sel||!dl)return;"
    "function fill(){var t=sel.value;dl.innerHTML='';"
    "if(['light','switch','sensor','scene','script','button'].indexOf(t)<0){hint.textContent=['weather','timer','sunrise','sunset','sun'].indexOf(t)>=0?'No entity needed for this type.':'';return;}"
    "hint.textContent='Loading '+t+' entities\\u2026';"
    "fetch('/ha/entities?domain='+t).then(function(r){if(!r.ok)throw 0;return r.text();}).then(function(x){"
    "var n=0;x.split('\\n').forEach(function(l){l=l.trim();if(!l)return;var p=l.split('|');"
    "var o=document.createElement('option');o.value=p[0];if(p[1])o.label=p[1];dl.appendChild(o);n++;});"
    "hint.textContent=n?(n+' entities \\u2013 type to filter, or paste any ID'):'None found \\u2013 you can still type an ID.';"
    "}).catch(function(){hint.textContent='Suggestions unavailable (check the HA connection) \\u2013 type the ID manually.';});}"
    "sel.addEventListener('change',fill);fill();"
    "})();</script>");
}

// JS for the "Add from area" block: loads areas into #areaSel, then on
// "Load entities" fetches /ha/area-entities and renders a checklist with
// group members pre-unchecked. Fails soft - the block just shows a notice.
String areaPickerScript() {
  return F(
    "<script>(function(){"
    "var AS=document.getElementById('areaSel');if(!AS)return;"
    "fetch('/ha/areas').then(function(r){if(!r.ok)throw 0;return r.text();}).then(function(t){"
    "AS.innerHTML='<option value=\"\">Select an area\\u2026</option>';"
    "t.split('\\n').forEach(function(l){l=l.trim();if(!l)return;var p=l.split('|');"
    "var o=document.createElement('option');o.value=p[0];o.textContent=p[1]||p[0];AS.appendChild(o);});"
    "}).catch(function(){AS.innerHTML='<option value=\"\">Areas unavailable (check HA connection)</option>';});"
    "window.loadArea=function(){var a=AS.value;if(!a)return;"
    "var dom=document.querySelector('input[name=areaDomain]:checked').value;"
    "document.getElementById('areaFormType').value=dom;"
    "var L=document.getElementById('areaList');var B=document.getElementById('areaAddBtn');"
    "B.style.display='none';L.textContent='Loading\\u2026';"
    "fetch('/ha/area-entities?area='+encodeURIComponent(a)+'&domain='+dom).then(function(r){if(!r.ok)throw 0;return r.text();}).then(function(t){"
    "var rows=t.split('\\n').map(function(l){return l.trim();}).filter(Boolean).map(function(l){var p=l.split('|');"
    "return{id:p[0],name:p[1]||p[0],members:(p[2]||'').split(',').filter(Boolean)};});"
    "if(!rows.length){L.textContent='No '+dom+' entities in that area.';return;}"
    "var covered={};rows.forEach(function(r){r.members.forEach(function(m){covered[m]=1;});});"
    "rows.sort(function(x,y){return (y.members.length>0)-(x.members.length>0)||x.name.localeCompare(y.name);});"
    "L.innerHTML='';rows.forEach(function(e){var g=e.members.length>0;"
    "var d=document.createElement('div');d.className='check';"
    "var cb=document.createElement('input');cb.type='checkbox';cb.name='eid';cb.value=e.id+'|'+e.name;"
    "cb.checked=g||!covered[e.id];"
    "var lb=document.createElement('label');lb.style.margin='0';"
    "lb.textContent=e.name+(g?(' (group of '+e.members.length+')'):(covered[e.id]?' \\u2013 covered by a group above':''));"
    "d.appendChild(cb);d.appendChild(lb);L.appendChild(d);});"
    "B.style.display='';"
    "}).catch(function(){L.textContent='Could not load entities (check HA connection).';});};"
    "})();</script>");
}

// Shared drag-to-reorder helper. makeSortable(listEl, onDrop): attaches to
// every .drag-handle inside listEl. While dragging, the row is lifted out
// of flow and tracks the pointer (.drag-float); a dashed placeholder
// (.drop-ph) marks where it will land and the other rows reflow around it.
// On drop, onDrop is called with the data-item values in the new order.
// Pointer Events - mouse + touch, no library.
String sortableScript() {
  return F(
    "<script>"
    "function makeSortable(list,onDrop){"
    "list.querySelectorAll('.drag-handle').forEach(function(h){"
    "h.addEventListener('pointerdown',function(e){e.preventDefault();"
    "var item=h.closest('[data-item]');var r=item.getBoundingClientRect();"
    "var offX=e.clientX-r.left,offY=e.clientY-r.top;"
    "var ph=document.createElement('div');ph.className='drop-ph';ph.style.height=r.height+'px';"
    "list.insertBefore(ph,item);"
    "item.classList.add('drag-float');item.style.width=r.width+'px';"
    "document.body.classList.add('dragging');"
    "function place(x,y){item.style.left=(x-offX)+'px';item.style.top=(y-offY)+'px';}"
    "place(e.clientX,e.clientY);h.setPointerCapture(e.pointerId);"
    "function mv(ev){place(ev.clientX,ev.clientY);var y=ev.clientY,before=null;"
    "var sibs=list.querySelectorAll('[data-item]:not(.drag-float)');"
    "for(var i=0;i<sibs.length;i++){var b=sibs[i].getBoundingClientRect();"
    "if(y<b.top+b.height/2){before=sibs[i];break;}}"
    "if(before)list.insertBefore(ph,before);else list.appendChild(ph);}"
    "function up(ev){try{h.releasePointerCapture(ev.pointerId);}catch(x){}"
    "h.removeEventListener('pointermove',mv);h.removeEventListener('pointerup',up);h.removeEventListener('pointercancel',up);"
    "list.insertBefore(item,ph);ph.remove();"
    "item.classList.remove('drag-float');item.style.width=item.style.left=item.style.top='';"
    "document.body.classList.remove('dragging');"
    "onDrop([].map.call(list.querySelectorAll('[data-item]'),function(el){return el.dataset.item;}));}"
    "h.addEventListener('pointermove',mv);h.addEventListener('pointerup',up);h.addEventListener('pointercancel',up);});});}"
    // Always reload after: the rows' Edit/Delete buttons carry positional
    // indices that must be re-rendered against the saved order.
    "function postOrder(url,order,extra){"
    "fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'order='+encodeURIComponent(order.join(','))+(extra||'')})"
    ".then(function(){location.reload();}).catch(function(){location.reload();});}"
    "</script>");
}

void handleRoot() {
  String h = pageHeaderHtml("Panelette Settings");
  h += "<h1>PANELETTE <span class='dim'>&mdash; " + htmlEscape(cfg.deviceName) + "</span></h1>";
  h += "<div class='muted' style='margin:-2px 0 8px'>v" FW_VERSION "</div>";

  if (gSaveNotice.length() > 0) {
    h += "<div class='notice'>" + htmlEscape(gSaveNotice) + "</div>";
    gSaveNotice = "";
  }

  // ---- Device / Home Assistant / Weather (three independent forms, one
  // shared handler - handleSaveDevice() is all hasArg()-guarded) ---------
  h += "<form method='POST' action='/save-device' class='dirty-guard'>";

  h += "<section class='card'><h2>Device</h2>";
  h += "<label>Device name (also the .local hostname)</label>";
  h += "<input name='deviceName' value='" + htmlEscape(cfg.deviceName) + "'>";
  h += "<div class='muted' style='margin-top:4px'>Letters, numbers and hyphens. Reach the panel at <b>" + htmlEscape(sanitizeHostname(cfg.deviceName)) + ".local</b>.</div>";
  h += "<label>Area / room name (shown on this panel's Home screen)</label>";
  h += "<input name='areaName' value='" + htmlEscape(cfg.pages[0].name) + "' placeholder='e.g. Family Room'>";
  h += "<div class='check'><input type='checkbox' id='use12h' name='use12h'" + String(cfg.use12Hour ? " checked" : "") +
       "><label for='use12h' style='margin:0'>Use 12-hour clock (AM/PM)</label></div>";
  h += "<div class='check'><input type='checkbox' id='flipScreen' name='flipScreen'" + String(cfg.flipScreen ? " checked" : "") +
       "><label for='flipScreen' style='margin:0'>Flip screen 180&deg; (USB port on the other side)</label></div>";
  h += "<label>Time Zone</label><select name='timezone'>";
  for (int i = 0; i < TZ_TABLE_COUNT; i++) {
    h += "<option value='" + String(TZ_TABLE[i].key) + "'";
    if (String(cfg.timezone) == TZ_TABLE[i].key) h += " selected";
    h += ">" + String(TZ_TABLE[i].label) + "</option>";
  }
  h += "</select>";
  h += "<label>Panel text size</label><select name='uiFontSize'>";
  h += "<option value='0'" + String(cfg.uiFontSize == 0 ? " selected" : "") + ">Small</option>";
  h += "<option value='1'" + String(cfg.uiFontSize == 1 ? " selected" : "") + ">Medium</option>";
  h += "<option value='2'" + String(cfg.uiFontSize == 2 ? " selected" : "") + ">Large</option>";
  h += "</select>";
  h += "<h3>Theme</h3>";
  h += "<label>Colour scheme</label><select name='colorScheme'>";
  for (int i = 0; i < COLOR_SCHEMES_COUNT; i++) {
    h += "<option value='" + String(COLOR_SCHEMES[i].key) + "'";
    if (String(cfg.colorScheme) == COLOR_SCHEMES[i].key) h += " selected";
    h += ">" + String(COLOR_SCHEMES[i].label) + "</option>";
  }
  h += "</select>";
  h += "<label>Typeface</label><select name='uiTypeface'>";
  h += "<option value='sans'" + String(strcmp(cfg.uiTypeface, "mono") ? " selected" : "") + ">Sans &mdash; Noto Sans, Title Case</option>";
  h += "<option value='mono'" + String(!strcmp(cfg.uiTypeface, "mono") ? " selected" : "") + ">Mono &mdash; Plex Mono, UPPERCASE</option>";
  h += "</select>";
  h += "<label>Corners</label><select name='cornerStyle'>";
  h += "<option value='rounded'" + String(strcmp(cfg.cornerStyle, "square") ? " selected" : "") + ">Rounded</option>";
  h += "<option value='square'" + String(!strcmp(cfg.cornerStyle, "square") ? " selected" : "") + ">Square</option>";
  h += "</select>";
  h += "<div class='muted' style='margin-top:6px;'>Dark/light is separate &mdash; toggle it on the panel's Status screen. All fonts are anti-aliased.</div>";
  h += "<button class='primary' type='submit'>Save device</button>";
  h += "</section></form>";

  h += "<form method='POST' action='/save-device' class='dirty-guard'>";
  h += "<section class='card'><h2>Home Assistant</h2>";
  h += "<label>Home Assistant URL</label>";
  h += "<input name='haUrl' value='" + htmlEscape(cfg.haUrl) + "'>";
  h += "<div class='muted' style='margin-top:4px'>Plain http:// on your LAN. Left at the default, the panel tries to find HA automatically.</div>";
  h += "<label>Long-lived access token</label>";
  h += "<input type='password' name='haToken' placeholder='" +
       String(strlen(cfg.haToken) > 0 ? "Saved (leave blank to keep it)" : "Paste your HA token") + "'>";
  {
    const char* pillCls = haConnState == HA_CONN_OK ? "ok"
                        : (haConnState == HA_CONN_AUTH_FAIL || haConnState == HA_CONN_UNREACHABLE) ? "bad"
                        : "warn";
    h += "<div style='margin-top:12px'>Connection: <span id='haStat' class='pill " + String(pillCls) + "'>" +
         htmlEscape(haConnLabel(haConnState)) + "</span> ";
    h += "<button type='button' onclick='haTest(this)'>Test</button></div>";
    h += "<div class='muted' style='margin-top:4px'>Tests the <em>saved</em> URL and token - Save first if you just changed them. A successful save also imports your time zone and location from HA.</div>";
  }
  h += "<div class='check' style='margin-top:16px'><input type='checkbox' id='haLive' name='haLiveUpdates'" +
       String(cfg.haLiveUpdates ? " checked" : "") + "><label for='haLive' style='margin:0'>Live updates &mdash; hold a WebSocket to HA for near-instant tile changes instead of 30-second polling</label></div>";
  h += "<div class='muted' style='margin-top:2px'>Status: <b>" + htmlEscape(haWsStatusText()) + "</b>";
  if (haWsNote.length() > 0) h += " &middot; " + htmlEscape(haWsNote);
  if (haWsEventCount > 0) h += " &middot; " + String(haWsEventCount) + " updates";
  if (haWsDropCount > 0)  h += " &middot; " + String(haWsDropCount) + " drops";
  h += "<br>Needs a plain http:// URL. Falls back to polling on any problem.</div>";
  h += "<button class='primary' type='submit'>Save Home Assistant</button>";
  h += "</section></form>";

  h += "<form method='POST' action='/save-device' class='dirty-guard'>";
  h += "<section class='card'><h2>Weather</h2>";
  h += "<div class='muted' style='margin-bottom:6px'>Coordinates are imported from Home Assistant on save. Edit any field to override.</div>";
  h += "<label>Location name (shown on the Forecast screen)</label>";
  h += "<input name='weatherName' value='" + htmlEscape(cfg.weatherLocationName) + "' placeholder='e.g. Portland, OR'>";
  h += "<div class='grid2' style='margin-top:8px'>";
  h += "<div><label>Latitude</label><input name='weatherLat' value='" + String(cfg.weatherLat, 4) + "'></div>";
  h += "<div><label>Longitude</label><input name='weatherLon' value='" + String(cfg.weatherLon, 4) + "'></div>";
  h += "</div>";
  h += "<button class='primary' type='submit'>Save weather</button>";
  h += "</section></form>";

  h += "<script>function haTest(b){var s=document.getElementById('haStat');b.disabled=true;s.textContent='Testing...';s.className='pill warn';"
       "fetch('/ha-test').then(r=>r.text()).then(t=>{s.textContent=t;s.className='pill '+(t=='Connected'?'ok':'bad');b.disabled=false;})"
       ".catch(function(){s.textContent='Test failed';s.className='pill bad';b.disabled=false;});}</script>";

  // ---- Network -------------------------------------------------------
  h += "<section class='card'><h2>Network</h2>";
  if (staticIpFellBack) {
    h += "<div class='notice'>The configured static IP didn't connect - the panel is on DHCP for now (current IP: " +
         htmlEscape(WiFi.localIP().toString()) + "). Fix the values below or switch to Automatic.</div>";
  }
  h += "<form method='POST' action='/save-network' class='dirty-guard'>";
  h += "<div class='check'><input type='radio' id='nmDhcp' name='netMode' value='dhcp'" + String(cfg.useStaticIp ? "" : " checked") +
       "><label for='nmDhcp' style='margin:0'>Automatic (DHCP)</label></div>";
  h += "<div class='check'><input type='radio' id='nmStatic' name='netMode' value='static'" + String(cfg.useStaticIp ? " checked" : "") +
       "><label for='nmStatic' style='margin:0'>Static IP</label></div>";
  h += "<div id='staticFields' style='display:none'>";
  h += "<label>IP address</label><input name='ipAddr' value='" + htmlEscape(cfg.ipAddr) + "' placeholder='192.168.1.50'>";
  h += "<label>Subnet mask</label><input name='subnet' value='" + htmlEscape(cfg.subnet) + "' placeholder='255.255.255.0'>";
  h += "<label>Gateway</label><input name='gateway' value='" + htmlEscape(cfg.gateway) + "' placeholder='192.168.1.1'>";
  h += "<div class='grid2' style='margin-top:8px'>";
  h += "<div><label>DNS 1 (optional)</label><input name='dns1' value='" + htmlEscape(cfg.dns1) + "' placeholder='1.1.1.1'></div>";
  h += "<div><label>DNS 2 (optional)</label><input name='dns2' value='" + htmlEscape(cfg.dns2) + "' placeholder='8.8.8.8'></div>";
  h += "</div>";
  h += "<div class='muted' style='margin-top:6px'>Leave DNS blank to use the gateway. If a static IP fails to connect, the panel falls back to DHCP so you can fix it.</div>";
  h += "</div>";
  h += "<button class='primary' type='submit'>Save network &amp; reboot</button>";
  h += "</form>";
  h += "<script>(function(){var f=document.getElementById('staticFields');"
       "function u(){f.style.display=document.querySelector('input[name=netMode]:checked').value=='static'?'':'none';}"
       "var r=document.getElementsByName('netMode');for(var i=0;i<r.length;i++)r[i].addEventListener('change',u);u();})();</script>";

  h += "<h3>Wi-Fi network</h3>";
  h += "<div class='muted'>Currently on <b>" + htmlEscape(WiFi.SSID()) + "</b>.</div>";
  if (gWifiCredSource == WCS_COMPILE) {
    h += "<div class='muted' style='margin-top:4px'>Wi-Fi is compiled into this build "
         "(<code>include/secrets.h</code>). Edit that and re-flash to change it.</div>";
  } else {
    char apName[24]; makeDefaultDeviceName(apName, sizeof(apName));
    h += "<div class='muted' style='margin:4px 0 6px'>Forget it to move the panel to a different "
         "network - it restarts into setup mode (its own <b>" + htmlEscape(apName) + "</b> network).</div>";
    h += "<form method='POST' action='/wifi/forget' onsubmit=\"return confirm('Forget this Wi-Fi network and restart into setup mode?');\">";
    h += "<button class='danger' type='submit'>Forget Wi-Fi &amp; restart</button></form>";
  }
  h += "</section>";

  // ---- Pages ----------------------------------------------------------
  h += "<section class='card'><h2>Pages</h2>";
  {
    h += "<div class='muted' style='margin-bottom:8px'>Drag a handle to reorder (Home stays first). Tick <b>Hide</b> to drop a page from the panel's swipe nav and footer. ";
    if (cfg.customPageOrder) {
      h += "Order is custom &ndash; ";
      h += "<form method='POST' action='/page/order-reset' style='display:inline'><button type='submit'>reset to default</button></form>";
    } else {
      h += "Default order: Home, rooms, Forecast, Timers, Status.";
    }
    h += "</div>";

    // Home is pinned at index 0 (several call sites assume cfg.pages[0] is
    // the home page), so it renders outside the sortable list.
    for (int i = 0; i < cfg.pageCount; i++) {
      PageConfig& pg = cfg.pages[i];
      bool isHome = (strcmp(pg.type, "home") == 0);
      String id = String(pg.id);

      if (i == 1) h += "<div id='pageList' class='sortable'>";

      bool tilePage = (isHome || strcmp(pg.type, "area") == 0);

      h += "<div class='row' data-item='" + id + "'>";
      if (!isHome) h += "<span class='drag-handle' title='Drag to reorder'>&#x283F;</span>";
      h += "<b>" + htmlEscape(pg.name) + "</b> <span class='muted'>(" + String(pg.type);
      if (tilePage) h += " &middot; " + String(pg.tileCount) + (pg.tileCount == 1 ? " tile" : " tiles");
      h += ")</span>";
      if (i == currentPageIndex) h += " <span class='pill ok'>on screen</span>";
      if (pg.hidden) h += " <span class='pill warn'>hidden</span>";
      h += "<br><a href='/page?id=" + id + "'>Manage tiles</a> &nbsp; ";
      h += "<form style='display:inline' method='POST' action='/page/rename'>";
      h += "<input type='hidden' name='id' value='" + id + "'>";
      h += "<input style='width:110px;display:inline-block' name='name' value='" + htmlEscape(pg.name) + "'>";
      h += "<button type='submit'>Rename</button></form>";
      if (pg.deletable) {
        h += " <form style='display:inline' method='POST' action='/page/delete' onsubmit=\"return confirm('Delete this page and its tiles?');\">";
        h += "<input type='hidden' name='id' value='" + id + "'>";
        h += "<button class='danger' type='submit'>Delete</button></form>";
      }
      if (pageCanHide(pg.type)) {
        h += " <form style='display:inline' method='POST' action='/page/set-hidden'>";
        h += "<input type='hidden' name='id' value='" + id + "'>";
        h += "<label style='font-size:13px;color:var(--dim)'><input type='checkbox' name='hidden' style='width:16px;height:16px;vertical-align:-3px' onchange='this.form.submit()'" +
             String(pg.hidden ? " checked" : "") + "> Hide</label></form>";
      }
      h += "</div>";
    }
    if (cfg.pageCount > 1) h += "</div>";

    if (cfg.pageCount < MAX_PAGES) {
      h += "<form method='POST' action='/page/add' style='margin-top:12px'>";
      h += "<label>Add area page</label><input name='name' placeholder='e.g. Kitchen'>";
      h += "<button class='primary' type='submit'>Add page</button></form>";
    } else {
      h += "<p class='muted'>Maximum of " + String(MAX_PAGES) + " pages reached.</p>";
    }
  }
  h += "</section>";

  // ---- Timers --------------------------------------------------------
  h += "<section class='card'><h2>Timers</h2>";

  h += "<h3>Timer expiry alerts</h3><form method='POST' action='/save-flash'>";
  h += "<div class='check'><input type='checkbox' id='marqueeEnabled' name='marqueeEnabled'" + String(cfg.marqueeEnabled ? " checked" : "") +
       "><label for='marqueeEnabled' style='margin:0'>Flash the red screen border until the timer is dismissed</label></div>";
  h += "<label>Pulse rate (ms between on/off)</label><input name='flashRate' value='" + String(cfg.flashPulseRateMs) + "'>";
  h += "<label>Pulse count (full on/off cycles)</label><input name='flashCount' value='" + String(cfg.flashPulseCount) + "'>";
  h += "<label>Dim-phase brightness (%) - the \"on\" phase is always 100%</label>";
  h += "<input name='flashBrightness' value='" + String(cfg.flashBrightnessPct) + "'>";
  h += "<div class='muted' style='margin-top:6px;'>Applies to dimmable lights; switches just toggle fully on/off.</div>";
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
        bool checked = selectedIds.indexOf(String(",") + eid + ",") >= 0;
        h += "<div class='check'><input type='checkbox' name='flashLight' value='" + htmlEscape(eid) + "'" +
             String(checked ? " checked" : "") + "><label style='margin:0'>" + htmlEscape(hlt.label) + "</label></div>";
      }
    }
    if (!anyLights) h += "<div class='muted'>Add light or switch tiles to the Home page first.</div>";
  }
  h += "<button class='primary' type='submit'>Save flash settings</button></form>";

  h += "<h3>Preset buttons</h3><form method='POST' action='/save-timers'>";
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
  h += "<button class='primary' type='submit'>Save presets</button></form>";
  h += "<form method='POST' action='/reset-timers' style='margin-top:8px' onsubmit=\"return confirm('Reset all 5 timer presets to 1/5/10/15/30 minutes?');\">";
  h += "<button type='submit'>Reset presets to defaults</button></form>";
  h += "</section>";

  // ---- Backup & Restore --------------------------------------------
  h += "<section class='card'><h2>Backup &amp; Restore</h2>";
  h += "<div class='seg' id='brSeg'>";
  h += "<button type='button' class='seg-on' data-pane='backup'>Back up</button>";
  h += "<button type='button' data-pane='restore'>Restore</button>";
  h += "</div>";
  h += "<div id='paneBackup'>";
  h += "<div class='muted' style='margin-bottom:8px'>Save every setting &mdash; device, pages, tiles, timers, network &mdash; to a JSON file on your computer.</div>";
  h += "<a class='btnlink' href='/export'>Download backup file</a>";
  h += "</div>";
  h += "<div id='paneRestore' hidden>";
  h += "<div class='muted' style='margin-bottom:8px'>Upload a backup file to replace <b>all</b> current settings. The panel reboots when it finishes.</div>";
  h += "<form method='POST' action='/import' enctype='multipart/form-data' onsubmit=\"return confirm('This replaces ALL current settings - device name, pages, tiles, everything. Continue?');\">";
  h += "<input type='file' name='configFile' accept='.json'>";
  h += "<button class='primary' type='submit' style='margin-top:10px'>Restore &amp; reboot</button></form>";
  h += "</div>";
  h += "<h3>Restart</h3>";
  h += "<form method='POST' action='/reboot' onsubmit=\"return confirm('Reboot the panel now?');\">";
  h += "<button type='submit'>Reboot panel</button></form>";
  h += "</section>";

  h += sortableScript();
  h += "<script>(function(){var L=document.getElementById('pageList');"
       "if(L)makeSortable(L,function(o){postOrder('/page/reorder',o);});})();</script>";
  // Backup / Restore segmented toggle: show one workflow, hide the other.
  h += "<script>(function(){var s=document.getElementById('brSeg');if(!s)return;"
       "s.addEventListener('click',function(e){var b=e.target.closest('button[data-pane]');if(!b)return;"
       "s.querySelectorAll('button').forEach(function(x){x.classList.toggle('seg-on',x===b);});"
       "document.getElementById('paneBackup').hidden=(b.dataset.pane!=='backup');"
       "document.getElementById('paneRestore').hidden=(b.dataset.pane!=='restore');});})();</script>";
  // Warn before leaving with unsaved edits in the long settings forms.
  h += "<script>(function(){var dirty=false;"
       "document.querySelectorAll('form.dirty-guard').forEach(function(f){"
       "f.addEventListener('input',function(){dirty=true;});"
       "f.addEventListener('submit',function(){dirty=false;});});"
       "window.addEventListener('beforeunload',function(e){if(dirty){e.preventDefault();e.returnValue='';}});})();</script>";

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

  if (pg->tileCount > 1) h += "<div class='muted' style='margin-bottom:6px'>Drag the handle to reorder tiles.</div>";
  h += "<div id='tileList' class='sortable'>";
  for (int i = 0; i < pg->tileCount; i++) {
    TileConfig& tl = pg->tiles[i];
    h += "<div class='row' data-item='" + String(i) + "'>";
    h += "<span class='drag-handle' title='Drag to reorder'>&#x283F;</span>";
    h += "<b>" + htmlEscape(tl.label) + "</b> <span class='muted'>(" + String(tl.type) + (tl.size == 2 ? ", wide" : "") + ")</span><br>";
    h += "<span class='muted'>" + htmlEscape(tl.entityId) + "</span><br>";
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
  h += "</div>";

  h += "<p>" + String(used) + " / 6 grid cells used.</p>";

  if (used < 6) {
    h += "<h2>Add tile</h2><form method='POST' action='/tile/add'>";
    h += "<input type='hidden' name='pageId' value='" + String(pg->id) + "'>";
    h += "<label>Type</label><select name='type'>";
    h += "<option value='light'>Light (toggle + dim)</option>";
    h += "<option value='switch'>Switch (toggle only)</option>";
    h += "<option value='sensor'>Sensor (read-only)</option>";
    h += "<option value='scene'>Scene (tap to activate)</option>";
    h += "<option value='script'>Script (tap to run)</option>";
    h += "<option value='button'>Button (tap to press)</option>";
    h += "<option value='weather'>Weather (icon + temperature)</option>";
    h += "<option value='timer'>Timer (countdown status)</option>";
    h += "<option value='sunrise'>Sunrise time</option>";
    h += "<option value='sunset'>Sunset time</option>";
    h += "<option value='sun'>Sunrise + Sunset (use a wide tile)</option>";
    h += "<option value='date'>Date (8/30/2026)</option>";
    h += "<option value='datewide'>Date + weekday (use a wide tile)</option>";
    h += "</select>";
    h += "<label>Label</label><input name='label' placeholder='e.g. Bedside Lamp'>";
    h += "<div class='check'><input type='checkbox' id='dateEuro' name='dateEuro'><label for='dateEuro' style='margin:0'>Date tiles: day/month order (30/8/2026) &mdash; unchecked is US month/day</label></div>";
    h += "<label>Entity ID (not used for Weather / Timer / Sun / Date tiles)</label>";
    h += "<input name='entityId' list='entlist' autocomplete='off' placeholder='e.g. light.living_room_lamp'>";
    h += "<datalist id='entlist'></datalist>";
    h += "<div class='muted' id='entHint'></div>";
    h += "<label>Size</label><select name='size'><option value='1'>1x1</option><option value='2'>1x2 (wide)</option></select>";
    h += "<button class='primary' type='submit'>Add tile</button></form>";
    h += entityPickerScript();

    // --- Add from a Home Assistant area -------------------------------
    h += "<h2>Add from a Home Assistant area</h2>";
    h += "<div class='muted' style='margin-bottom:8px'>Pick an area, load its lights or switches, and tick the ones you want. Group entities are listed first; their members are pre-unchecked since adding the group already covers them.</div>";
    h += "<label>Area</label><select id='areaSel'><option value=''>Loading areas...</option></select>";
    h += "<div style='margin-top:8px'>";
    h += "<label class='check' style='display:inline-flex'><input type='radio' name='areaDomain' value='light' checked> Lights</label>";
    h += "<label class='check' style='display:inline-flex;margin-left:16px'><input type='radio' name='areaDomain' value='switch'> Switches</label>";
    h += "</div>";
    h += "<button type='button' onclick='loadArea()'>Load entities</button>";
    h += "<form id='areaForm' method='POST' action='/tile/add-bulk' style='margin-top:10px'>";
    h += "<input type='hidden' name='pageId' value='" + String(pg->id) + "'>";
    h += "<input type='hidden' name='type' id='areaFormType' value='light'>";
    h += "<div id='areaList' class='muted'>&mdash;</div>";
    h += "<button class='primary' type='submit' id='areaAddBtn' style='display:none'>Add checked tiles</button>";
    h += "</form>";
    h += areaPickerScript();
  } else {
    h += "<p>This page's grid is full.</p>";
  }

  h += sortableScript();
  h += "<script>(function(){var L=document.getElementById('tileList');"
       "if(L)makeSortable(L,function(o){postOrder('/tile/reorder',o,'&pageId=" + String(pg->id) + "');});})();</script>";

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
  const char* types[13] = {"light", "switch", "sensor", "scene", "script", "button",
                           "weather", "timer", "sunrise", "sunset", "sun", "date", "datewide"};
  const char* typeLabels[13] = {"Light (toggle + dim)", "Switch (toggle only)", "Sensor (read-only)",
                                "Scene (tap to activate)", "Script (tap to run)", "Button (tap to press)",
                                "Weather (icon + temperature)", "Timer (countdown status)",
                                "Sunrise time", "Sunset time", "Sunrise + Sunset (use a wide tile)",
                                "Date (8/30/2026)", "Date + weekday (use a wide tile)"};
  for (int i = 0; i < 13; i++) {
    h += "<option value='" + String(types[i]) + "'";
    if (strcmp(tl.type, types[i]) == 0) h += " selected";
    h += ">" + String(typeLabels[i]) + "</option>";
  }
  h += "</select>";

  h += "<label>Label</label><input name='label' value='" + htmlEscape(tl.label) + "'>";
  h += "<div class='check'><input type='checkbox' id='dateEuro' name='dateEuro'" + String(tl.dateEuro ? " checked" : "") +
       "><label for='dateEuro' style='margin:0'>Date tiles: day/month order (30/8/2026) &mdash; unchecked is US month/day</label></div>";
  h += "<label>Entity ID (not used for Weather / Timer / Sun / Date tiles)</label>";
  h += "<input name='entityId' list='entlist' autocomplete='off' value='" + htmlEscape(tl.entityId) + "'>";
  h += "<datalist id='entlist'></datalist>";
  h += "<div class='muted' id='entHint'></div>";

  h += "<label>Size</label><select name='size'>";
  h += "<option value='1'" + String(tl.size == 1 ? " selected" : "") + ">1x1</option>";
  h += "<option value='2'" + String(tl.size == 2 ? " selected" : "") + ">1x2 (wide)</option>";
  h += "</select>";

  h += "<button class='primary' type='submit'>Save changes</button></form>";
  h += entityPickerScript();

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
        tl.dateEuro = server.hasArg("dateEuro");

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

  newName = sanitizeHostname(newName); // also generates a default if it ends up empty
  newUrl.trim();
  while (newUrl.endsWith("/")) newUrl.remove(newUrl.length() - 1);
  newToken.trim();
  newAreaName.trim();
  if (newAreaName.length() == 0) newAreaName = "Home";

  // The Device / Home Assistant / Weather cards are three separate forms
  // that all POST here; each only carries its own fields. Guard anything
  // that isn't hasArg()-safe (bare checkboxes) behind a field unique to
  // that form so a partial submit can't clear the others' settings.
  bool deviceForm  = server.hasArg("deviceName");
  bool haForm      = server.hasArg("haUrl") || server.hasArg("haToken");
  bool weatherForm = server.hasArg("weatherName") || server.hasArg("weatherLat") || server.hasArg("weatherLon");

  strlcpy(cfg.deviceName, newName.c_str(), sizeof(cfg.deviceName));
  strlcpy(cfg.haUrl, newUrl.c_str(), sizeof(cfg.haUrl));
  if (newToken.length() > 0) strlcpy(cfg.haToken, newToken.c_str(), sizeof(cfg.haToken));
  strlcpy(cfg.pages[0].name, newAreaName.c_str(), sizeof(cfg.pages[0].name));
  if (haForm) {
    cfg.haLiveUpdates = server.hasArg("haLiveUpdates");
    haWsStop(); // reconnect fresh with the current URL / token / toggle
  }
  if (deviceForm) {
    cfg.use12Hour = server.hasArg("use12h"); // unchecked checkboxes are simply absent from the POST body
    cfg.flipScreen = server.hasArg("flipScreen");
    applyScreenRotation();
  }
  strlcpy(cfg.timezone, sanitizeTimezoneKey(server.hasArg("timezone") ? server.arg("timezone") : String(cfg.timezone)).c_str(), sizeof(cfg.timezone));
  applyTimezone();
  cfg.uiFontSize = (uint8_t)constrain(server.hasArg("uiFontSize") ? server.arg("uiFontSize").toInt() : cfg.uiFontSize, 0, 2);
  if (server.hasArg("uiTypeface")) {
    String tf = server.arg("uiTypeface");
    if (tf == "sans" || tf == "mono") strlcpy(cfg.uiTypeface, tf.c_str(), sizeof(cfg.uiTypeface));
  }
  if (server.hasArg("colorScheme")) {
    String c = server.arg("colorScheme");
    for (int i = 0; i < COLOR_SCHEMES_COUNT; i++)
      if (c == COLOR_SCHEMES[i].key) { strlcpy(cfg.colorScheme, c.c_str(), sizeof(cfg.colorScheme)); break; }
  }
  if (server.hasArg("cornerStyle")) {
    String c = server.arg("cornerStyle");
    if (c == "rounded" || c == "square") strlcpy(cfg.cornerStyle, c.c_str(), sizeof(cfg.cornerStyle));
  }
  applyTypeface();
  applyCornerStyle();
  applyTheme(cfg.darkTheme);
  // Bulb color picker and web UI font picker removed from the settings
  // page for now - cfg.bulbColorKey stays at its default ("amber") and
  // cfg.webFontChoice is unused (Poppins is hardcoded in pageHeaderHtml).
  // The underlying tables/config fields are left in place in case either
  // gets a UI control again later.

  if (server.hasArg("weatherLat")) cfg.weatherLat = server.arg("weatherLat").toFloat();
  if (server.hasArg("weatherLon")) cfg.weatherLon = server.arg("weatherLon").toFloat();
  if (server.hasArg("weatherName")) {
    String wn = server.arg("weatherName");
    wn.trim();
    if (wn.length() > 0) strlcpy(cfg.weatherLocationName, wn.c_str(), sizeof(cfg.weatherLocationName));
  }
  // Location may have changed - drop the cached forecast so it refetches promptly.
  if (weatherForm) {
    weatherKnown = false;
    lastWeatherFetch = 0;
  }

  saveConfig();
  pageDirty = true;

  // Probe HA and, when reachable, import time zone + location from it -
  // only when the Home Assistant card was the form submitted.
  // Result is surfaced as a one-shot banner on the settings page.
  gSaveNotice = "Settings saved.";
  if (haForm && strlen(cfg.haUrl) > 0 && strlen(cfg.haToken) > 0) {
    HaConnState s = haProbeConnection();
    haConnState = s;
    lastHaCheckMs = millis();
    if (s == HA_CONN_OK) {
      gSaveNotice += " Home Assistant connection OK.";
      if (haFetchAndApplyConfig()) {
        saveConfig();
        gSaveNotice += " Imported time zone and location from Home Assistant.";
      }
    } else if (s == HA_CONN_AUTH_FAIL) {
      gSaveNotice += " Home Assistant rejected the token - check it was pasted in full.";
    } else {
      gSaveNotice += " Could not reach Home Assistant at that URL.";
    }
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleHaTest() {
  HaConnState s = haProbeConnection();
  haConnState = s;
  lastHaCheckMs = millis();
  server.send(200, "text/plain", haConnLabel(s));
}

void sendRebootPage(const String& heading, const String& detail) {
  String p = pageHeaderHtml("Rebooting");
  p += "<h1>" + htmlEscape(heading) + "</h1><p>" + detail;
  p += " The panel is unreachable for ~15 seconds. <a href='/'>Back to settings</a>.</p>";
  p += htmlFooter;
  server.send(200, "text/html", p);
  delay(700);
  ESP.restart();
}

void handleReboot() {
  sendRebootPage("Rebooting", "");
}

void handleWifiForget() {
  wifiCredsClearNvs();
  Serial.println("[wifi] credentials cleared - restarting into setup mode");
  char apName[24]; makeDefaultDeviceName(apName, sizeof(apName));
  sendRebootPage("Wi-Fi forgotten",
                 "Restarting into setup mode. Join the panel's own <b>" + String(apName) +
                 "</b> Wi-Fi network to reconnect it.");
}

void handleSaveNetwork() {
  bool wantStatic = server.hasArg("netMode") && server.arg("netMode") == "static";

  if (wantStatic) {
    String ip = server.arg("ipAddr");   ip.trim();
    String sn = server.arg("subnet");   sn.trim();
    String gw = server.arg("gateway");  gw.trim();
    String d1 = server.arg("dns1");     d1.trim();
    String d2 = server.arg("dns2");     d2.trim();

    String err = validateStaticNet(ip, sn, gw);
    IPAddress t;
    if (err == "" && d1.length() && !t.fromString(d1)) err = "Primary DNS is not a valid IP.";
    if (err == "" && d2.length() && !t.fromString(d2)) err = "Secondary DNS is not a valid IP.";

    if (err.length()) {
      gSaveNotice = "Network settings not saved - " + err;
      server.sendHeader("Location", "/");
      server.send(303);
      return;
    }

    cfg.useStaticIp = true;
    strlcpy(cfg.ipAddr,  ip.c_str(), sizeof(cfg.ipAddr));
    strlcpy(cfg.subnet,  sn.c_str(), sizeof(cfg.subnet));
    strlcpy(cfg.gateway, gw.c_str(), sizeof(cfg.gateway));
    strlcpy(cfg.dns1,    d1.c_str(), sizeof(cfg.dns1));
    strlcpy(cfg.dns2,    d2.c_str(), sizeof(cfg.dns2));
  } else {
    cfg.useStaticIp = false;
  }

  saveConfig();
  sendRebootPage("Network settings saved",
                 wantStatic ? "Rebooting to apply the static IP. It may come back at the new address - check the panel's Status screen."
                            : "Rebooting to switch back to DHCP.");
}

void handleSaveFlash() {
  // The whole "Timer expiry alerts" card is one form; flashRate is always
  // present in it, so use that as the marker that these fields were posted
  // (an unchecked checkbox / no ticked lights are simply absent).
  if (server.hasArg("flashRate")) {
    cfg.flashPulseRateMs = constrain(server.arg("flashRate").toInt(), 100, 5000);
    cfg.marqueeEnabled = server.hasArg("marqueeEnabled");

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
  }

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

void handleResetTimers() {
  for (int i = 0; i < 5; i++) cfg.timerPresetSec[i] = DEFAULT_TIMER_PRESETS_SEC[i];
  saveConfig();
  pageDirty = true;
  gSaveNotice = "Timer presets reset to defaults (1, 5, 10, 15, 30 min).";
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
    pg.hidden = false;
    pg.tileCount = 0;
    cfg.pageCount++;

    // In auto mode the new area page sorts in ahead of Forecast/Timers/
    // Status, so page indices shift; in custom mode it just stays at the
    // end for the user to drag. Either way, keep the panel on the page it
    // was showing and drop the now-misaligned per-tile runtime cache.
    String curId = (currentPageIndex >= 0 && currentPageIndex < cfg.pageCount)
                     ? String(cfg.pages[currentPageIndex].id) : "";
    if (!cfg.customPageOrder) sortPages();
    for (int i = 0; i < cfg.pageCount; i++) {
      if (curId.length() && curId == cfg.pages[i].id) { currentPageIndex = i; break; }
    }
    for (int p = 0; p < MAX_PAGES; p++)
      for (int t = 0; t < MAX_TILES; t++)
        tileRuntime[p][t] = TileRuntime();
    pageDirty = true;
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
        tl.dateEuro = server.hasArg("dateEuro");
        pg->tileCount++;
        saveConfig();
        pageDirty = true;
      }
    }
  }
  server.sendHeader("Location", "/page?id=" + server.arg("pageId"));
  server.send(303);
}

// --- HA discovery endpoints (consumed by JS in the tile forms) ---------
// All return newline-delimited "field|field" text, or an empty body with a
// non-200 status on failure, so the browser side can degrade to plain
// manual entry.

void handleHaEntities() {
  String domain = server.arg("domain");
  if (domain != "light" && domain != "switch" && domain != "sensor" &&
      domain != "scene" && domain != "script" && domain != "button") {
    server.send(400, "text/plain", "");
    return;
  }
  String tmpl = "{% for s in states." + domain +
                " | sort(attribute='name') %}{{ s.entity_id }}|{{ s.name }}\n{% endfor %}";
  String out;
  if (!haRenderTemplate(tmpl, out)) { server.send(502, "text/plain", ""); return; }
  server.send(200, "text/plain; charset=utf-8", out);
}

void handleHaAreas() {
  String tmpl = "{% for a in areas() %}{{ a }}|{{ area_name(a) }}\n{% endfor %}";
  String out;
  if (!haRenderTemplate(tmpl, out)) { server.send(502, "text/plain", ""); return; }
  server.send(200, "text/plain; charset=utf-8", out);
}

void handleHaAreaEntities() {
  String area = server.arg("area");
  String domain = server.arg("domain");
  if (!isSafeSlug(area) || (domain != "light" && domain != "switch")) {
    server.send(400, "text/plain", "");
    return;
  }
  // Third field = comma-joined member ids for group entities (empty
  // otherwise), so the browser can pre-uncheck members a group covers.
  String tmpl = "{% for e in area_entities('" + area + "') if e.startswith('" + domain + ".') %}"
                "{{ e }}|{{ state_attr(e,'friendly_name') or e }}|"
                "{{ (state_attr(e,'entity_id') or []) | join(',') }}\n{% endfor %}";
  String out;
  if (!haRenderTemplate(tmpl, out)) { server.send(502, "text/plain", ""); return; }
  server.send(200, "text/plain; charset=utf-8", out);
}

// Bulk add from the "Add from area" checklist. Checkbox values are
// "entity_id|Friendly Name". Adds 1x1 tiles, skips duplicates, stops at
// the 6-cell grid limit.
void handleTileAddBulk() {
  String pageId = server.arg("pageId");
  PageConfig* pg = findPageById(pageId);
  if (pg) {
    String type = server.hasArg("type") ? server.arg("type") : "light";
    if (type != "light" && type != "switch") type = "light";

    int used = 0;
    for (int i = 0; i < pg->tileCount; i++) used += pg->tiles[i].size;

    int n = server.args();
    for (int a = 0; a < n && pg->tileCount < MAX_TILES && used < 6; a++) {
      if (server.argName(a) != "eid") continue;
      String v = server.arg(a);
      int bar = v.indexOf('|');
      String eid = (bar >= 0 ? v.substring(0, bar) : v);
      String label = (bar >= 0 ? v.substring(bar + 1) : "");
      eid.trim();
      label.trim();
      if (eid.length() == 0) continue;

      bool dup = false;
      for (int i = 0; i < pg->tileCount; i++) {
        if (eid == pg->tiles[i].entityId) { dup = true; break; }
      }
      if (dup) continue;

      if (label.length() == 0) label = prettyFromEntityId(eid);

      TileConfig& tl = pg->tiles[pg->tileCount];
      strlcpy(tl.type, type.c_str(), sizeof(tl.type));
      strlcpy(tl.label, label.c_str(), sizeof(tl.label));
      strlcpy(tl.entityId, eid.c_str(), sizeof(tl.entityId));
      tl.size = 1;
      tl.dateEuro = false;
      pg->tileCount++;
      used++;
    }
    saveConfig();
    pageDirty = true;
  }
  server.sendHeader("Location", "/page?id=" + pageId);
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

// Parse a comma-separated "order" arg into tokens. Returns the count.
static int parseOrderList(const String& order, String out[], int maxOut) {
  int count = 0, start = 0;
  while (start <= (int)order.length() && count < maxOut) {
    int comma = order.indexOf(',', start);
    String tok = (comma < 0) ? order.substring(start) : order.substring(start, comma);
    tok.trim();
    if (tok.length()) out[count++] = tok;
    if (comma < 0) break;
    start = comma + 1;
  }
  return count;
}

// Drag-to-reorder: "order" is the tile indices (0-based, original
// positions) in their new order. Validated as a permutation before applying.
void handleTileReorder() {
  bool ok = false;
  PageConfig* pg = findPageById(server.arg("pageId"));
  if (pg && server.hasArg("order")) {
    String toks[MAX_TILES];
    int count = parseOrderList(server.arg("order"), toks, MAX_TILES);
    if (count == pg->tileCount) {
      bool seen[MAX_TILES];
      for (int i = 0; i < MAX_TILES; i++) seen[i] = false;
      bool valid = true;
      int idxs[MAX_TILES];
      for (int i = 0; i < count && valid; i++) {
        idxs[i] = toks[i].toInt();
        if (idxs[i] < 0 || idxs[i] >= pg->tileCount || seen[idxs[i]]) valid = false;
        else seen[idxs[i]] = true;
      }
      if (valid) {
        TileConfig tmp[MAX_TILES];
        for (int i = 0; i < count; i++) tmp[i] = pg->tiles[idxs[i]];
        for (int i = 0; i < count; i++) pg->tiles[i] = tmp[i];
        int pageIdx = (int)(pg - cfg.pages);
        for (int t = 0; t < MAX_TILES; t++) tileRuntime[pageIdx][t] = TileRuntime();
        saveConfig();
        pageDirty = true;
        ok = true;
      }
    }
  }
  server.send(ok ? 200 : 400, "text/plain", ok ? "ok" : "bad");
}

// Drag-to-reorder pages. "order" is the new sequence of every page id
// EXCEPT Home (Home is pinned at index 0 - several call sites assume
// cfg.pages[0] is the home page). Switches the config to custom page
// order, which loadConfig() then leaves untouched.
void handlePageReorder() {
  bool ok = false;
  if (server.hasArg("order")) {
    String ids[MAX_PAGES];
    int count = parseOrderList(server.arg("order"), ids, MAX_PAGES);

    // Expect exactly every non-home page, each once.
    int movable = cfg.pageCount - 1; // minus Home
    if (movable > 0 && count == movable) {
      PageConfig ordered[MAX_PAGES];
      int on = 0;
      bool valid = true;
      for (int k = 0; k < count && valid; k++) {
        int found = -1;
        for (int i = 1; i < cfg.pageCount; i++)
          if (ids[k] == cfg.pages[i].id) { found = i; break; }
        // reject unknown ids and duplicates (a dup would re-match the same slot)
        if (found < 0) { valid = false; break; }
        for (int j = 0; j < on; j++)
          if (strcmp(ordered[j].id, cfg.pages[found].id) == 0) { valid = false; break; }
        if (valid) ordered[on++] = cfg.pages[found];
      }
      if (valid && on == movable) {
        String curId = (currentPageIndex >= 0 && currentPageIndex < cfg.pageCount)
                         ? String(cfg.pages[currentPageIndex].id) : "";
        for (int i = 0; i < movable; i++) cfg.pages[i + 1] = ordered[i];
        cfg.customPageOrder = true;
        for (int i = 0; i < cfg.pageCount; i++)
          if (curId.length() && curId == cfg.pages[i].id) currentPageIndex = i;
        for (int p = 0; p < MAX_PAGES; p++)
          for (int t = 0; t < MAX_TILES; t++) tileRuntime[p][t] = TileRuntime();
        saveConfig();
        pageDirty = true;
        ok = true;
      }
    }
  }
  server.send(ok ? 200 : 400, "text/plain", ok ? "ok" : "bad");
}

// Toggle a page's "hidden from the on-device nav" flag (checkbox auto-POSTs).
void handlePageSetHidden() {
  if (server.hasArg("id")) {
    PageConfig* pg = findPageById(server.arg("id"));
    if (pg && pageCanHide(pg->type)) {
      pg->hidden = server.hasArg("hidden");
      if (pg->hidden && (int)(pg - cfg.pages) == currentPageIndex) currentPageIndex = 0;
      saveConfig();
      pageDirty = true;
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

// Restore the automatic page order (Home, areas, Forecast, Timers, Status).
void handlePageOrderReset() {
  String curId = (currentPageIndex >= 0 && currentPageIndex < cfg.pageCount)
                   ? String(cfg.pages[currentPageIndex].id) : "";
  cfg.customPageOrder = false;
  sortPages();
  for (int i = 0; i < cfg.pageCount; i++)
    if (curId.length() && curId == cfg.pages[i].id) currentPageIndex = i;
  for (int p = 0; p < MAX_PAGES; p++)
    for (int t = 0; t < MAX_TILES; t++) tileRuntime[p][t] = TileRuntime();
  saveConfig();
  pageDirty = true;
  server.sendHeader("Location", "/");
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
    h += "<p>That file didn't look like a valid Panelette config export. Nothing was changed.</p>";
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

  String filename = "panelette-" + devicePart + "-" + dateStr + ".json";

  server.sendHeader("Content-Disposition", "attachment; filename=" + filename);
  server.send(200, "application/json", json);
}

// ---- Wi-Fi provisioning portal (only wired up while gProvisioning) ------

String provPageHtml(const String& body) {
  return "<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Panelette Wi-Fi setup</title><style>"
         "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#0e1211;"
         "color:#e7ece9;margin:0;padding:28px 20px;line-height:1.5}"
         ".c{max-width:380px;margin:0 auto}"
         "h1{font-size:22px;margin:0 0 4px}.d{color:#90a099;font-size:14px;margin:0 0 20px}"
         "label{display:block;font-size:13px;color:#90a099;margin:14px 0 4px}"
         "input,select{width:100%;box-sizing:border-box;padding:11px 12px;font-size:15px;border-radius:9px;"
         "border:1px solid #2c3532;background:#1d2523;color:#e7ece9}"
         "button{width:100%;margin-top:20px;padding:12px;font-size:15px;font-weight:600;border:0;"
         "border-radius:9px;background:#e3a94e;color:#0e1211;cursor:pointer}"
         "a{color:#e3a94e}.n{background:#1d2523;border:1px solid #2c3532;border-radius:9px;"
         "padding:12px 14px;font-size:14px;margin-top:16px}</style></head><body><div class='c'>" +
         body + "</div></body></html>";
}

void handleProvRoot() {
  String b = "<h1>Panelette</h1><p class='d'>Connect the panel to your Wi-Fi.</p>";
  b += "<form method='POST' action='/wifi/save'>";
  b += "<label>Network</label>";
  b += "<input name='ssid' list='nets' autocomplete='off' placeholder='network name' required>";
  b += "<datalist id='nets'>" + gProvScanHtml + "</datalist>";
  b += "<label>Password <span style='color:#63726b'>(leave blank if open)</span></label>";
  b += "<input name='pass' type='password' autocomplete='off'>";
  b += "<button type='submit'>Save &amp; connect</button></form>";
  b += "<div class='n'>Don't see your network? <a href='/wifi/scan'>Rescan</a></div>";
  server.send(200, "text/html", provPageHtml(b));
}

void handleProvScan() {
  provScanNetworks();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleProvSave() {
  String ssid = server.arg("ssid"); ssid.trim();
  String pass = server.arg("pass");
  if (ssid.length() == 0 || ssid.length() > 32) {
    server.send(400, "text/html", provPageHtml("<h1>Panelette</h1><p>Please enter a network name. <a href='/'>Back</a></p>"));
    return;
  }

  // Verify before saving - a wrong password shouldn't get written to NVS.
  Serial.printf("[prov] trying '%s'\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long t = millis();
  while (millis() - t < 14000) {
    wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED && WiFi.localIP() != IPAddress((uint32_t)0)) break;
    if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL) break;
    delay(200);
  }
  bool ok = (WiFi.status() == WL_CONNECTED) && (WiFi.localIP() != IPAddress((uint32_t)0));

  if (!ok) {
    WiFi.disconnect(false, true);
    Serial.println("[prov] connect failed - not saved");
    String b = "<h1>Couldn't connect</h1><p class='d'>Check the password for <b>" + ssid + "</b> and try again.</p>"
               "<a class='n' href='/' style='display:block;text-decoration:none'>&larr; Back to setup</a>";
    server.send(200, "text/html", provPageHtml(b));
    return; // stay in provisioning
  }

  wifiCredsSaveToNvs(ssid.c_str(), pass.c_str());
  Serial.printf("[prov] connected as %s - saved, restarting\n", WiFi.localIP().toString().c_str());
  String b = "<h1>Connected</h1><p class='d'>The panel joined <b>" + ssid + "</b> and is restarting.</p>";
  server.send(200, "text/html", provPageHtml(b));
  delay(700);
  ESP.restart();
}

// Anything else (incl. the OS captive-portal probe URLs) -> the setup page.
void handleProvCaptive() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
  server.send(302, "text/plain", "");
}

void setupWebServer() {
  if (gProvisioning) {
    server.on("/", handleProvRoot);
    server.on("/wifi/save", HTTP_POST, handleProvSave);
    server.on("/wifi/scan", handleProvScan);
    server.onNotFound(handleProvCaptive);
    server.begin();
    return;
  }

  server.on("/", handleRoot);
  server.on("/ha-test", handleHaTest);
  server.on("/page", handlePageManage);
  server.on("/save-device", HTTP_POST, handleSaveDevice);
  server.on("/save-network", HTTP_POST, handleSaveNetwork);
  server.on("/wifi/forget", HTTP_POST, handleWifiForget);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/save-flash", HTTP_POST, handleSaveFlash);
  server.on("/save-timers", HTTP_POST, handleSaveTimers);
  server.on("/reset-timers", HTTP_POST, handleResetTimers);
  server.on("/page/add", HTTP_POST, handlePageAdd);
  server.on("/page/rename", HTTP_POST, handlePageRename);
  server.on("/page/delete", HTTP_POST, handlePageDelete);
  server.on("/page/reorder", HTTP_POST, handlePageReorder);
  server.on("/page/order-reset", HTTP_POST, handlePageOrderReset);
  server.on("/page/set-hidden", HTTP_POST, handlePageSetHidden);
  server.on("/tile/add", HTTP_POST, handleTileAdd);
  server.on("/tile/add-bulk", HTTP_POST, handleTileAddBulk);
  server.on("/tile/delete", HTTP_POST, handleTileDelete);
  server.on("/tile/move", HTTP_POST, handleTileMove);
  server.on("/tile/reorder", HTTP_POST, handleTileReorder);
  server.on("/tile/edit", handleTileEditForm);
  server.on("/tile/update", HTTP_POST, handleTileUpdate);
  server.on("/ha/entities", handleHaEntities);
  server.on("/ha/areas", handleHaAreas);
  server.on("/ha/area-entities", handleHaAreaEntities);
  server.on("/export", handleExport);
  server.on("/import", HTTP_POST, handleImportComplete, handleImportUpload);
  server.begin();
}

// =========================================================
// SETUP / LOOP
// =========================================================
void setup() {
  Serial.begin(115200);
  delay(80);
  Serial.printf("\n=== %s %s  (build %s %s) ===\n", FW_NAME, FW_VERSION, __DATE__, __TIME__);

  pinMode(BACKLIGHT_PIN, OUTPUT);
  analogWrite(BACKLIGHT_PIN, 255);
  tft.init();
  tft.setRotation(2);
  tft.fillScreen(fixColor565(TFT_BLACK)); // display renders every colour inverted - see fixColor565

  // ESP Web Tools probes for Improv within ~1 s of opening the port (which
  // resets us) and does not retry - so blast CURRENT_STATE for the first
  // ~2 s, before the slow filesystem / font init, so one lands in its
  // detect window. improvLoop() keeps a slower announce going after this.
  for (int i = 0; i < 14; i++) { improvSendState(IMP_STATE_READY); improvLoop(); delay(150); }

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
  improvLoop();
  if (!loadConfig()) {
    setDefaultConfig();
    saveConfig();
  }
  applyScreenRotation(); // now that cfg.flipScreen is known
  applyTypeface();       // load the .vlw font (sans / mono)
  improvLoop();
  applyCornerStyle();
  applyTheme(cfg.darkTheme);
  applyTimezone();

  WiFi.mode(WIFI_STA);
  applyNetworkConfig(); // static IP, if configured - must precede WiFi.begin()

  WifiCredSource cs = wifiResolveCreds();
  Serial.printf("Wi-Fi credentials: %s\n",
                cs == WCS_COMPILE ? "from secrets.h" :
                cs == WCS_STORED  ? "from device storage" : "NONE (setup needed)");

  if (cs != WCS_NONE) {
    WiFi.begin(gWifiSsid, gWifiPass);
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
      improvDelay(250);
    }

    // Safety net: a bad static IP (right format, wrong for this LAN) would
    // otherwise lock the user out. Retry once on DHCP so the web UI stays
    // reachable; the stored config is left as-is.
    if (WiFi.status() != WL_CONNECTED && cfg.useStaticIp) {
      Serial.println("Static IP failed to connect - falling back to DHCP for this session.");
      staticIpFellBack = true;
      WiFi.config((uint32_t)0, (uint32_t)0, (uint32_t)0);
      WiFi.disconnect();
      delay(100);
      WiFi.begin(gWifiSsid, gWifiPass);
      wifiStart = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
        improvDelay(250);
      }
    }
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
      improvDelay(100);
    }
    applyTimezone();

    MDNS.begin(cfg.deviceName);
    Serial.print("mDNS hostname: ");
    Serial.print(cfg.deviceName);
    Serial.println(".local");

    // If the HA URL is still the built-in default, try to find HA on the
    // LAN. Not persisted here - only saved once the user saves settings.
    if (strcmp(cfg.haUrl, HA_URL_DEFAULT) == 0) {
      String found;
      if (discoverHaUrl(found)) {
        strlcpy(cfg.haUrl, found.c_str(), sizeof(cfg.haUrl));
        Serial.print("mDNS: discovered Home Assistant at ");
        Serial.println(cfg.haUrl);
      }
    }
  } else {
    Serial.println("Not connected - entering Wi-Fi setup mode.");
    startProvisioning();
  }

  setupWebServer();

  pageDirty = true;
}

// Drains the one-slot tap command queue. Called from loop() right after the
// touch pass so the blocking POST never sits between a release and the
// contact-bounce that follows it.
void flushPendingHaCommand() {
  if (pendingHaKind == PHA_NONE) return;
  PendingHaKind k = pendingHaKind;
  pendingHaKind = PHA_NONE;
  if (k == PHA_ONOFF)           haSendCommand(pendingHaEntity, pendingHaOn);
  else if (k == PHA_ACTIVATE)   haActivate(pendingHaEntity);
  else if (k == PHA_BRIGHTNESS) haSendBrightness(pendingHaEntity, pendingHaPct);
}

// Whole-loop replacement while gProvisioning: serve the captive portal,
// keep the setup screen up, and (if we have credentials that failed at
// boot) periodically retry them, rebooting into normal mode on success.
void handleProvisioning() {
  server.handleClient();
  dnsServer.processNextRequest();

  if (!gProvScreenDrawn) {
    drawProvisioningScreen(gWifiCredSource != WCS_NONE);
    gProvScreenDrawn = true;
  }

  if (gWifiCredSource != WCS_NONE && WiFi.softAPgetStationNum() == 0 &&
      millis() - gProvRetryMs > 60000) {
    gProvRetryMs = millis();
    Serial.println("[prov] retrying stored Wi-Fi...");
    WiFi.begin(gWifiSsid, gWifiPass);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 12000) delay(200);
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress((uint32_t)0)) {
      Serial.println("[prov] connected - restarting into normal mode");
      delay(300);
      ESP.restart();
    }
  }
}

void loop() {
  improvLoop(); // browser-installer serial handshake - runs in every mode

  if (gProvisioning) { handleProvisioning(); return; }

  server.handleClient();
  updateTimerState();
  updateFlashSequence();

  // A page just hidden from the web UI (or a bad saved index) must not
  // stay on screen - fall back to Home, which can never be hidden.
  if (currentPageIndex < 0 || currentPageIndex >= cfg.pageCount ||
      cfg.pages[currentPageIndex].hidden) {
    currentPageIndex = 0;
    pageDirty = true;
  }

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
  flushPendingHaCommand(); // run the tap's HA call now, outside the touch pass
  haWsLoop();              // optional HA WebSocket live updates
  ensureWeather();

  // Re-probe HA immediately when the Status page is opened, so its readout
  // isn't up to a full check-interval stale.
  static int haPageWatch = -1;
  if (currentPageIndex != haPageWatch) {
    haPageWatch = currentPageIndex;
    if (strcmp(cfg.pages[currentPageIndex].type, "status") == 0) lastHaCheckMs = 0;
  }
  ensureHaCheck();

  if (rebootArmed && millis() - rebootArmedMs > REBOOT_CONFIRM_WINDOW_MS) {
    rebootArmed = false;
    pageDirty = true;
  }

  if (pageDirty) {
    drawCurrentPageFull();
    // Live updates keep tileRuntime current for every page, so a page-open
    // poll is only needed when the socket isn't carrying us.
    if (!haWsActive()) pollCurrentPageTiles();
    lastPollMs = millis();
  } else {
    updateCurrentPageDynamic();
    // Live updates push state as it changes, so drop the poll to a slow
    // backstop while the socket is up (catches anything it missed).
    unsigned long pollGap = haWsActive() ? 150000UL : POLL_INTERVAL_MS;
    if (millis() - lastPollMs > pollGap) {
      pollCurrentPageTiles();
      lastPollMs = millis();
    }
  }

  updateMarqueeBorder(); // flashes on top every tick once a timer has expired
}
