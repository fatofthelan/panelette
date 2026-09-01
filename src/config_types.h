#pragma once
#include <Arduino.h>

// =========================================================
// CONFIG DATA MODEL
// =========================================================
// Kept in this header (rather than the .ino) because the Arduino IDE
// auto-generates function prototypes and inserts them all in one block
// right after the #include lines, before any of the .ino's own code.
// If these structs lived in the .ino, functions that take them as
// parameters (e.g. TileRuntime&) would get a prototype referencing a
// type the compiler hasn't seen yet. Including this header up top
// guarantees the types exist before that auto-prototype block does.
#define MAX_PAGES 8
#define MAX_TILES 6 // grid only has 6 cells (2x3)

struct TileConfig {
  char type[12];      // "light" | "switch" | "sensor" | "blank"
  char label[24];
  char entityId[48];  // e.g. "light.living_room_lamp"
  uint8_t size;        // 1 = single cell, 2 = wide (spans both columns)
  bool dateEuro;       // "date"/"datewide" tiles: false = M/D/Y (US, default), true = D/M/Y
};

struct PageConfig {
  char id[16];         // stable short id, e.g. "home", "status", "area_a1b2"
  char name[24];        // display name, editable
  char type[12];        // "home" | "area" | "forecast" | "timers" | "status"
  bool deletable;
  bool hidden;          // true = skipped in the on-device footer + swipe nav (Home/Status can't be hidden)
  uint8_t tileCount;
  TileConfig tiles[MAX_TILES];
};

struct DeviceConfig {
  char deviceName[24];
  char haUrl[80];
  char haToken[200];
  bool haLiveUpdates;  // experimental: hold a HA WebSocket for push state updates instead of polling
  bool darkTheme;
  bool use12Hour;
  bool flipScreen;   // true = rotate the display 180 deg (USB port on the other side)
  char timezone[24];
  uint8_t uiFontSize;  // 0 = Small, 1 = Medium (default), 2 = Large
  char uiTypeface[16]; // theme axis: "sans" (Noto Sans) | "mono" (Plex Mono, UPPERCASE + rules)
  char colorScheme[12]; // theme axis: "cool" | "warm" | "phosphor" | "neutral"
  char cornerStyle[10]; // theme axis: "rounded" | "square"
  bool uiBoldText;      // faux-bold: draws text twice, offset by 1px
  char bulbColorKey[16]; // which preset the "on" bulb glow uses - see BULB_COLORS in the .ino
  float weatherLat;
  float weatherLon;
  char weatherLocationName[40];
  char webFontChoice[16]; // key into WEB_FONTS - which font the browser-side settings UI uses
  char flashLightIds[200]; // comma-separated entity_ids selected for timer-expiry flashing
  int flashPulseRateMs;    // ms between on/off transitions
  int flashPulseCount;     // number of full on-off cycles
  int flashBrightnessPct;  // dim phase target for light.* entities; "on" phase is always 100
  bool flashOnExpire;      // persisted state of the Timers-page "Flash Lights" toggle (default on)
  bool marqueeEnabled;     // true = flash the red screen border when a timer expires (default on)
  int timerPresetSec[5];   // editable Timers-page preset buttons, in seconds (supports sub-minute presets)
  bool customPageOrder;    // false = auto (Home, areas, Forecast, Timers, Status); true = user drag order kept as-is
  bool useStaticIp;        // false = DHCP (default); true = use the fields below (applied at boot, before WiFi.begin)
  char ipAddr[16];         // dotted-quad strings, "" when unused
  char subnet[16];
  char gateway[16];
  char dns1[16];           // optional
  char dns2[16];           // optional
  uint8_t pageCount;
  PageConfig pages[MAX_PAGES];
};

// Per-tile runtime state (not persisted). Indexed [pageIndex][tileIndex].
struct TileRuntime {
  bool known = false;
  bool on = false;
  bool unavailable = false; // HA reported "unavailable"/"unknown" (e.g. bulb powered off at the switch)
  int brightnessPct = 100;
  String lastRawState = "";
  time_t lastFetch = 0;
  String cacheKey = "";
};

// One row of the time-zone table (also carries the default weather
// location for that zone). Lives here rather than the .ino for the same
// auto-prototype reason as the structs above.
struct TzEntry {
  const char* key;
  const char* label;
  const char* posix; // POSIX TZ string, handles DST automatically
  float lat;
  float lon;
  const char* cityLabel; // default weather location for this time zone
};

// One row of the web-UI font table. googleFontsParam is null for the
// system-default option (no external font to load).
struct WebFontEntry {
  const char* key;
  const char* label;
  const char* googleFontsParam;
  const char* cssFamily;
};

// HA connectivity, shown on the Status page and the web UI. In this header
// (not the .ino) because haConnLabel() takes it as a parameter and the
// Arduino auto-prototype block would otherwise reference it before it's
// defined - see "auto-prototype gotcha" in CLAUDE.md.
enum HaConnState {
  HA_CONN_UNKNOWN,      // not probed yet this session
  HA_CONN_UNCONFIGURED, // no URL or no token stored
  HA_CONN_OK,           // GET /api/ returned 200
  HA_CONN_AUTH_FAIL,    // 401/403 - token wrong or lacks scope
  HA_CONN_UNREACHABLE   // no WiFi, connect/DNS failure, or other error
};
