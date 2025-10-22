# ESP32 Encrypted Notes (AP + SD)

A self-hosted notes app for ESP32 that spins up a Wi‑Fi Access Point (AP) and serves a web UI. Notes and folders are persisted either on SD card (encrypted) if available or fall back to ESP32 flash (unencrypted).

- **Sketch**: `notes.ino`
- **Board**: ESP32 Dev Module (ESP32 Arduino core)

## Features
- **Wi‑Fi AP** with password: configurable `ssid` and `password`.
- **HTTP server** with a simple UI and REST endpoints.
- **Encrypted storage on SD** using AES-256 (key derived via SHA‑256) with hex encoding.
- **Flash fallback** when no SD detected.
- **Migration**: moves existing flash data to SD automatically when SD is first found.
- **Light sleep + reduced CPU frequency** for lower power usage.

## What you must configure
Edit `notes.ino` and update these constants before flashing:
- **`ssid`**: AP name you want to broadcast.
- **`password`**: AP password.
- **`ENCRYPTION_KEY`**: a strong passphrase; any length is accepted (code derives a 32‑byte key via SHA‑256).
- (Optional) **SD pin definitions** if your wiring differs:
  - `SD_CS` (default 5)
  - `SD_MOSI` (default 23)
  - `SD_MISO` (default 19)
  - `SD_SCK` (default 18)
- (Optional) Power tuning:
  - `#define LIGHT_SLEEP_ENABLED true`
  - `const int SLEEP_DURATION = 100;`  // microseconds in loop
  - `setCpuFrequencyMhz(80);`          // called in `setup()`

Important: If you change `ENCRYPTION_KEY` after you already saved notes to SD, the existing data will no longer decrypt. Keep a backup or migrate first.

## Hardware and wiring
Required:
- ESP32 dev board (e.g., ESP32‑DevKitC)
- microSD card module (SPI)
- microSD card (FAT/FAT32)

Default SPI wiring used by the sketch (adjust if needed):
- **ESP32 3V3** → SD module VCC (use 3.3V modules; if your module is 5V‑tolerant with regulator/level shifting, 5V can be used per its docs)
- **ESP32 GND** → SD module GND
- **ESP32 GPIO23** → SD MOSI
- **ESP32 GPIO19** → SD MISO
- **ESP32 GPIO18** → SD SCK
- **ESP32 GPIO5** → SD CS

Notes:
- Many microSD breakout boards include level shifters/regulators. Follow your module’s pin labels (CS, MOSI, MISO, SCK/CLK, VCC, GND).
- Use short wires to improve SPI signal integrity.

## Build and flash
- Install the ESP32 Arduino core (Board Manager: "esp32" by Espressif).
- Select Board: ESP32 Dev Module (or your specific ESP32 board).
- Open `notes.ino`, set the values under “What you must configure”.
- Compile and upload.

## First run and usage
1. Power the ESP32 with SD card inserted.
2. The serial monitor will print the AP IP address (typically `192.168.4.1`).
3. On your phone/PC, connect to the AP (`ssid`/`password`).
4. Open the shown IP in a browser. Create folders and notes from the UI.

## How it works (high level)
- **AP + Server**: `WiFi.softAP(ssid, password)` creates the AP, `WebServer server(80)` serves the UI and endpoints.
- **Routes**:
  - `/` serves the HTML UI from `handleRoot()`.
  - `/api/status` returns storage status (SD vs Flash, encrypted flag).
  - `/api/folders` (GET/POST), `/api/folders/delete` manage folders.
  - `/api/notes` (GET/POST), `/api/notes/delete` manage notes.
  - `/api/save` persists current in‑RAM state to the active backend.
- **Storage selection**:
  - If `SD.begin(SD_CS)` and root exists, SD is used (encrypted).
  - Otherwise, ESP32 `Preferences` (flash) is used (unencrypted).
- **Encryption (SD mode)**:
  - `deriveKey(ENCRYPTION_KEY)` uses SHA‑256 to produce a 32‑byte key.
  - AES‑256 ECB encrypts 16‑byte blocks; output is stored as uppercase hex.
  - Files: `/folders.enc` and `/notes.enc` on SD.
- **Migration**:
  - On first SD detect, reads existing flash items and writes them to SD (encrypted), then clears flash.

## Power efficiency and battery notes
Built‑in optimizations in `notes.ino`:
- **Lower CPU frequency**: `setCpuFrequencyMhz(80);` reduces active power.
- **Light sleep**: `WiFi.setSleep(true)` and short delays in `loop()` via `SLEEP_DURATION`.

Tips you can apply:
- **Increase `SLEEP_DURATION`** for lower CPU duty cycle when idle (balance UI responsiveness).
- **Reduce CPU further**: 80 MHz is a good balance; 40 MHz can work but may affect Wi‑Fi responsiveness.
- **Turn off AP when not needed**: add logic to disable Wi‑Fi during long idle periods and re‑enable on a button/interrupt.
- **Use efficient power hardware**: a good buck converter and a quality Li‑ion cell can materially extend runtime.

Approximate behavior (very rough guidance):
- AP + server active can draw tens of mA to 100+ mA depending on load and RF conditions.
- Light sleep and lower CPU help, but AP mode still consumes more than STA idle or deep sleep.

## Troubleshooting
- **SD not detected**: check wiring, CS pin, card format, module voltage level.
- **Garbled SD data**: ensure `ENCRYPTION_KEY` hasn’t changed; hex files are not human‑readable by design.
- **UI unreachable**: connect to the correct AP; confirm serial IP (usually `192.168.4.1`).
- **Performance sluggish**: lower `SLEEP_DURATION` or increase CPU MHz.

## Security notes
- AP password and encryption key are stored in firmware; do not publish them.
- Changing `ENCRYPTION_KEY` after writing encrypted data prevents decryption of old data.

---
If you want, I can add an optional migration helper to re‑encrypt SD data when rotating `ENCRYPTION_KEY`.
