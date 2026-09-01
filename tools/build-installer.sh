#!/usr/bin/env bash
# Builds the firmware and stages the ESP Web Tools browser installer under
# docs/ (served by GitHub Pages): the flash parts + a manifest.json.
#
# Multi-part (not a single merged .bin) on purpose: ESP Web Tools then
# writes only the four regions it's given, so an *update* (no full erase)
# leaves NVS - the stored Wi-Fi credentials - and the LittleFS config
# partition untouched.
#
# Usage:  tools/build-installer.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PIO="${PIO:-pio}"
command -v "$PIO" >/dev/null || PIO="$HOME/.platformio/penv/bin/pio"

BUILD=".pio/build/esp32dev"
BOOT_APP0="${BOOT_APP0:-$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin}"
DEST="docs/firmware"

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

echo ">> building firmware ${VERSION} (no secrets.h)"
"$PIO" run -e esp32dev

echo ">> staging parts -> ${DEST}/"
mkdir -p "$DEST"
cp "$BUILD/bootloader.bin"  "$DEST/bootloader.bin"
cp "$BUILD/partitions.bin"  "$DEST/partitions.bin"
cp "$BOOT_APP0"             "$DEST/boot_app0.bin"
cp "$BUILD/firmware.bin"    "$DEST/firmware.bin"

echo ">> writing docs/manifest.json"
cat > docs/manifest.json <<JSON
{
  "name": "Panelette",
  "version": "${VERSION}",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "firmware/bootloader.bin", "offset": 4096 },
        { "path": "firmware/partitions.bin", "offset": 32768 },
        { "path": "firmware/boot_app0.bin",  "offset": 57344 },
        { "path": "firmware/firmware.bin",   "offset": 65536 }
      ]
    }
  ]
}
JSON

echo ">> done:"
ls -la "$DEST"/ docs/manifest.json
