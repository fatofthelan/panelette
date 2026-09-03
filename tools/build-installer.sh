#!/usr/bin/env bash
# Builds every CYD panel variant and stages the ESP Web Tools browser installer
# under docs/ (served by GitHub Pages): per-variant flash parts + manifests.
#
# The CYD ships with several different display panels; docs/index.html lets the
# user pick, which swaps the ESP Web Tools manifest. See platformio.ini.
#
# Multi-part (not a single merged .bin) on purpose: ESP Web Tools then writes
# only the four regions it's given, so an *update* (no full erase) leaves NVS -
# the stored Wi-Fi credentials - and the LittleFS config partition untouched.
#
# Usage:  tools/build-installer.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PIO="${PIO:-pio}"
command -v "$PIO" >/dev/null || PIO="$HOME/.platformio/penv/bin/pio"

BOOT_APP0="${BOOT_APP0:-$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin}"
DEST="docs/firmware"

# env name  ->  human label shown in the picker's <option>
ENVS=(esp32dev cyd_ili9341 cyd_st7789)

VERSION="$(grep -oE '#define FW_VERSION "[^"]+"' src/main.ino | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')"
[ -n "$VERSION" ] || { echo "could not read FW_VERSION from src/main.ino" >&2; exit 1; }

# The installer image must NOT carry anyone's Wi-Fi - build as if secrets.h
# didn't exist (this is also how CI builds it).
SECRETS="include/secrets.h"
if [ -f "$SECRETS" ]; then
  mv "$SECRETS" "${SECRETS}.installer-bak"
  trap 'mv "${SECRETS}.installer-bak" "$SECRETS" 2>/dev/null || true' EXIT
  echo ">> moved $SECRETS aside for the build"
fi

write_manifest() {
  # $1 = output path, $2 = firmware subdir under docs/firmware/
  cat > "$1" <<JSON
{
  "name": "Panelette",
  "version": "${VERSION}",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "firmware/$2/bootloader.bin", "offset": 4096 },
        { "path": "firmware/$2/partitions.bin", "offset": 32768 },
        { "path": "firmware/$2/boot_app0.bin",  "offset": 57344 },
        { "path": "firmware/$2/firmware.bin",   "offset": 65536 }
      ]
    }
  ]
}
JSON
}

for ENV in "${ENVS[@]}"; do
  echo ">> building ${ENV} (firmware ${VERSION}, no secrets.h)"
  "$PIO" run -e "$ENV"

  BUILD=".pio/build/$ENV"
  OUT="$DEST/$ENV"
  mkdir -p "$OUT"
  cp "$BUILD/bootloader.bin" "$OUT/bootloader.bin"
  cp "$BUILD/partitions.bin" "$OUT/partitions.bin"
  cp "$BOOT_APP0"            "$OUT/boot_app0.bin"
  cp "$BUILD/firmware.bin"   "$OUT/firmware.bin"

  write_manifest "docs/manifest-${ENV}.json" "$ENV"
done

# Back-compat: bare manifest.json === the original board's build.
cp docs/manifest-esp32dev.json docs/manifest.json

echo ">> done:"
ls -la "$DEST"/*/ docs/manifest*.json
