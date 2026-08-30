# HA Panel - Installation Guide

A multi-page Home Assistant touch control panel for the **ESP32-2432S028R**
("CYD" - Cheap Yellow Display), a ~$15 2.8" 240x320 touchscreen board.

Tap to toggle lights and switches, press-and-drag on a light to dim it,
swipe between pages, see a live weather forecast, and run kitchen-style
timers that can flash your lights when they finish.

---

## What you need

### Hardware
- An **ESP32-2432S028R** board (the common "CYD" / Cheap Yellow Display, the
  2.8" resistive-touch version). This firmware is tuned for that exact board.
- A **USB data cable** that fits the board (micro-USB or USB-C depending on
  your revision). Many cheap cables are charge-only and will not work - if
  the board doesn't show up as a serial port, try another cable first.
- A computer (Windows, macOS, or Linux) for the one-time flashing step.

A fresh panel names itself `HAPanelXXXX` (the last 4 hex digits of its MAC),
so two panels on one network won't clash. You can rename it in the web UI.

### Software (installed once, on your computer)
- **Visual Studio Code** - https://code.visualstudio.com
- The **PlatformIO IDE** extension for VS Code (installed from inside VS Code,
  steps below)
- **USB serial driver** - only some setups need this:
  - The CYD uses a **CH340** USB chip.
  - **Linux:** usually works out of the box.
  - **macOS / Windows:** if the board doesn't appear as a serial port after
    plugging it in, install the CH340 driver from
    https://www.wch-ic.com/downloads/CH341SER_ZIP.html (Windows) or
    https://github.com/WCHSoftGroup/ch34xser_macos (macOS), then reboot.

### Home Assistant
- A running Home Assistant instance on the **same network** as the panel.
- Its address, e.g. `http://homeassistant.local:8123` or `http://192.168.1.50:8123`.
- Ability to create a **Long-Lived Access Token** (any admin user can - steps below).

### Network
- **2.4 GHz Wi-Fi.** The ESP32 cannot connect to 5 GHz networks. If your
  router uses one name for both bands, the panel will find the 2.4 GHz one
  automatically; if the bands have separate names, use the 2.4 GHz one.
- WPA2 is recommended. WPA3-only networks may not connect.

---

## Step 1 - Get the code

**If you have `git`:**
```
git clone <REPO-URL> ha-panel
cd ha-panel
```

**If you don't:** on the project's GitHub page, click **Code -> Download ZIP**,
then unzip it somewhere permanent (not your Downloads folder).

---

## Step 2 - Install VS Code + PlatformIO

1. Install and open **Visual Studio Code**.
2. Open the **Extensions** panel (the square icon in the left sidebar, or
   `Ctrl+Shift+X` / `Cmd+Shift+X`).
3. Search for **PlatformIO IDE**, click **Install**. Wait for it to finish
   (it downloads a toolchain in the background - this can take a few minutes
   the first time). Reload VS Code if it asks.
4. **File -> Open Folder** and choose the `ha-panel` folder from Step 1.
   PlatformIO will detect the project and do more first-time setup - give it
   a minute until the bottom status bar stops showing activity.

---

## Step 3 - Enter your Wi-Fi credentials

The panel needs your Wi-Fi name and password compiled into the firmware.

1. In VS Code's file explorer, open the **`include`** folder.
2. Find **`secrets.h.example`**. Right-click it -> **Copy**, then right-click
   the `include` folder -> **Paste**. Rename the copy to exactly
   **`secrets.h`**.
3. Open `secrets.h` and replace the placeholders:
   ```c
   #define WIFI_SSID     "your-wifi-ssid"
   #define WIFI_PASSWORD "your-wifi-password"
   ```
   Keep the quotes. Save the file.

`secrets.h` is git-ignored, so your password will not be committed if you
later push changes.

> **There is no on-screen Wi-Fi setup.** If you get the password wrong, the
> panel simply shows "No WiFi connection" and you'll need to fix `secrets.h`
> and re-flash (Step 5). Double-check it now.

---

## Step 4 - (Optional) set the default HA address

The panel names itself `HAPanelXXXX` automatically and you can rename it (and
set the HA URL) from the web UI after first boot, so this step is optional.

To pre-set the Home Assistant address, open **`src/main.ino`** and edit:
```c
const char* HA_URL_DEFAULT = "http://homeassistant.local:8123";
```
Leave the `:8123` port unless you've changed it. Save the file.

---

## Step 5 - Flash the panel

1. Plug the CYD into your computer with the USB **data** cable.
2. At the bottom of the VS Code window, in the blue PlatformIO status bar,
   click the **-> (right arrow)** icon - "PlatformIO: Upload".
   - First build downloads more libraries and takes a few minutes.
     Subsequent builds are ~10 seconds.
3. Watch the terminal at the bottom. Success ends with:
   ```
   Writing at 0x... (100 %)
   Hash of data verified.
   [SUCCESS]
   ```
4. The panel reboots on its own and shows the **Home** screen with a clock.

If upload fails, see **Troubleshooting** below.

---

## Step 6 - Find the panel on your network

The panel needs no further USB connection - it's now on Wi-Fi.

- On the panel, swipe left/right to the **Status** page. It shows:
  - **WiFi:** Connected / Disconnected
  - **IP:** e.g. `192.168.1.73`
  - **Host:** the panel's name, e.g. `HAPanelA1B2.local`
- On your computer/phone browser (same network), open `http://<that host>`
  (e.g. `http://HAPanelA1B2.local`)
  - If your device or router doesn't resolve `.local` names, use the IP from
    the Status page instead: `http://192.168.1.73`

You should see the **HA Panel Settings** page.

---

## Step 7 - Create a Home Assistant access token

1. In Home Assistant, click your **username** (bottom-left) to open your
   profile.
2. Open the **Security** tab (older HA: scroll to the bottom of the profile
   page).
3. Under **Long-Lived Access Tokens**, click **Create Token**.
4. Name it something like `HA Panel` and click OK.
5. **Copy the token now** - HA shows it only once. It's a long string of
   letters and numbers.

---

## Step 8 - Connect the panel to Home Assistant

On the panel's web page, in the **Home Assistant** section:

1. **Home Assistant URL** - the panel tries to auto-fill this by finding
   Home Assistant on your network. If it's blank or wrong, enter it
   manually, e.g. `http://homeassistant.local:8123` or
   `http://192.168.1.50:8123` (no trailing slash).
2. **Long-lived access token** - paste the token from Step 7.
3. Set the **Area / room name** shown on the Home screen. (Time zone and
   location are pulled from Home Assistant automatically on save - adjust
   them here afterward if needed.)
4. Click **Save**. A banner reports whether the panel reached Home
   Assistant. You can re-check any time with the **Test** button, or by
   tapping the **HA** row on the panel's Status page.

> Use a plain `http://` address on your local network. `https://` with a
> self-signed certificate will not work; a valid public certificate does.

---

## Step 9 - Add your controls

On the panel's web page:

1. Click **Manage tiles** on a page (the **Home** page exists by default), or
   **Add area page** for a new room.
2. **Add tile:**
   - **Type:**
     - *Light* - tap toggles, press-and-drag sets brightness
     - *Switch* - tap toggles
     - *Sensor* - read-only value
     - *Scene / Script / Button* - tap runs it once
     - *Weather* - current icon + temperature (no entity needed)
     - *Timer* - jumps to the Timers page (no entity needed)
   - **Label** - what shows on the tile
   - **Entity ID** - start typing and pick from the list (the panel loads
     your entities of that type from Home Assistant), or paste an ID. If the
     list is empty, check the HA connection - you can still type the ID.
   - **Size** - `1x1` or `1x2` (wide, spans both columns)
3. **Add from a Home Assistant area** (below Add tile): pick an area, load its
   lights or switches, and tick what you want. If you use group helpers
   (e.g. an "Office" group of 3 bulbs), the group is listed first and its
   members are pre-unchecked - so you add one tile, not three.
4. Each page holds up to **6 tiles** (2 columns x 3 rows; a wide tile uses
   two cells). **Drag the handle (⠿)** to reorder tiles, or delete them.
   On the main settings page you can drag **pages** into any order the same
   way (Home stays first). The default order is Home, your rooms, Forecast,
   Timers, Status - a "Reset to default order" button restores it.

To find entity IDs by hand: Home Assistant **Developer Tools -> States**
(e.g. `light.living_room_lamp`, `switch.coffee_maker`, `scene.movie_night`).

Changes take effect on the panel immediately.

---

## Using the panel

- **Swipe left/right** - change pages, or tap an icon in the bottom bar. The
  highlighted icon is the page you're on.
- **Tap a light/switch tile** - toggle it.
- **Press and hold a light tile, then drag** - brightness slider.
- **Forecast page** - 5-day weather from Open-Meteo (no account needed;
  location follows your time zone, or set it manually in the web UI).
- **Timers page** - tap a preset (editable in the web UI) or set a custom
  time; optionally tick "flash lights" so configured lights pulse when it
  finishes.
- **Status page** - connection info, a **Dark/Light** theme toggle, and
  **Reboot** (tap twice to confirm).

---

## Updating the firmware later

1. Get the new code (`git pull`, or download the new ZIP over your folder -
   but **keep your `include/secrets.h`**).
2. Plug in over USB, click **Upload** again.

Your pages, tiles and HA token are stored on the panel's flash filesystem and
**survive a firmware update** - they are not part of the code.

### Backup / move your setup
On the web page, use **Export** to download a `hapanel-<name>-<date>.json`
file, and **Import** to restore it (or copy a configuration to another panel).

---

## Troubleshooting

**The board doesn't show up / upload fails with "could not open port" or
"no serial data received"**
- Use a different USB cable (charge-only cables are the #1 cause).
- Install the CH340 driver (see "What you need").
- Close anything else using the serial port (Arduino IDE, a serial monitor).
- Some boards need you to **hold the BOOT button** while upload starts, then
  release it once "Connecting..." appears.
- Try a slower speed: open `platformio.ini` and change `upload_speed` to
  `230400`.

**Upload succeeds but the screen is black**
- The backlight is on pin 21; a fully black screen with the board otherwise
  working usually means a wrong display setting for your board revision. This
  firmware is built for the **ST7789** version of the CYD. If yours is older
  (ILI9341), open `platformio.ini` and change `-D ST7789_DRIVER=1` to
  `-D ILI9341_2_DRIVER=1`, remove the `-D TFT_RGB_ORDER=TFT_BGR` line, and
  re-upload.

**Screen works but colors look wrong (blues look orange, etc.)**
- See `CLAUDE.md` -> "the display renders every color as its bitwise
  complement". This board family has a known quirk the firmware already
  compensates for; if yours differs, that section explains the knobs.

**Panel shows "No WiFi connection"**
- Re-check `include/secrets.h` (exact SSID and password, keep the quotes),
  save, re-upload.
- Confirm it's a **2.4 GHz** network.
- Check the router isn't blocking new devices (MAC filtering).

**Web page won't load at the `.local` name**
- Use the IP address from the panel's Status page instead.
- Make sure your computer/phone is on the same network/VLAN as the panel.

**Tiles show but nothing happens when tapped / sensors show "?"**
- Re-check the **HA URL** (no trailing slash, correct port) and that the
  **token** was pasted fully.
- Confirm the **entity ID** exactly matches Developer Tools -> States.
- The token's user must have permission to control that entity.

**Touch is offset or unresponsive**
- The touch calibration constants near the top of `src/main.ino`
  (`TOUCH_X_MIN` ... `TOUCH_Y_MAX`) are tuned for a typical CYD. If yours is
  noticeably off, those values can be adjusted; open an issue with what you're
  seeing.

**Watch the panel's debug output**
- In VS Code's PlatformIO status bar, click the **plug icon** ("Serial
  Monitor"). It runs at 115200 baud and prints Wi-Fi status, the assigned IP,
  and any errors.

---

## Privacy / network notes

- The panel talks only to your Home Assistant instance and to
  `api.open-meteo.com` for weather. It has no cloud account and phones home
  nowhere else.
- The HA token is stored in plain text on the panel's flash and sent over
  your local network - keep the panel on a trusted network, and revoke the
  token in HA if a panel is lost.
