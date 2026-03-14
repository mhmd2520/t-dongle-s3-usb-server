# T-Dongle-S3 Smart USB Drive — Project Reference

## Hardware

| Component  | Spec                                      |
|------------|-------------------------------------------|
| MCU        | ESP32-S3 Xtensa LX7 dual-core @ 240 MHz  |
| RAM        | 8 MB PSRAM + 512 KB internal SRAM        |
| Flash      | 16 MB                                     |
| Storage    | microSD via SPI (32 GB)                   |
| Display    | ST7735 0.96" IPS, 80×160 px, 65k color   |
| RGB LED    | APA102 — DATA=GPIO40, CLK=GPIO39 (SPI, BGR order) |
| Wireless   | WiFi 802.11 b/g/n, BT 5.0                |
| USB        | USB-A — device mode only (no host)        |
| Power      | 5V USB — no battery, no hardware thermal protection |

---

## Critical Design Constraint: USB MSC + WiFi Cannot Coexist

ESP32-S3 WiFi and TinyUSB MSC **cannot run simultaneously** — they share RF/PHY resources and
destabilize each other. Additionally, when a PC mounts the SD card via USB MSC it owns the FAT
filesystem; concurrent ESP32 writes cause **filesystem corruption**.

**Solution: Dual-Mode Architecture**

```
┌─────────────────────┐              ┌──────────────────────────┐
│   USB DRIVE MODE    │              │    NETWORK / SERVER MODE │
│                     │              │                          │
│  TinyUSB MSC active │◄── SWITCH ──►│  WiFi active             │
│  SD card → PC       │              │  HTTPS Web UI + Download │
│  WiFi DISABLED      │              │  USB MSC DISABLED        │
└─────────────────────┘              └──────────────────────────┘
```

- Switching tears down active stack, initializes the other
- Mode saved to NVS; restored on next boot
- Use cases don't overlap in practice — this is not a real limitation

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    ESP32-S3 T-Dongle-S3                        │
│                                                                 │
│  ┌─────────────────────┐   ┌────────────────────────────────┐  │
│  │   USB DRIVE MODE    │   │       NETWORK MODE             │  │
│  │  TinyUSB MSC        │   │  WiFi Stack                    │  │
│  │  SD card → host     │   │  ├─ HTTPS Web UI (port 443)    │  │
│  │  (read/write)       │   │  ├─ HTTP→HTTPS redirect (80)   │  │
│  └─────────────────────┘   │  ├─ Webhook API (/api/download) │  │
│                             │  ├─ Telegram Bot (HTTPS)       │  │
│                             │  └─ mDNS (usbdrive.local)     │  │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │               SHARED STORAGE LAYER                        │  │
│  │  SD Card (FAT32) — full 32 GB                            │  │
│  │  NVS Flash — WiFi creds, settings, themes, last mode     │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                     LCD UI LAYER                          │  │
│  │  TFT_eSPI — ST7735 80×160 px                             │  │
│  │  Mode status / WiFi info / IP / progress bar / QR code   │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Library Stack

| Purpose         | Library                      | Notes                              |
|-----------------|------------------------------|------------------------------------|
| USB MSC         | `USBMSC.h` (arduino-esp32)   | Built into Arduino Core 2.x        |
| HTTPS Server    | fhessel/esp32_https_server   | Port 443 + port 80 redirect; self-signed cert |
| Telegram Bot    | AsyncTelegram2 (cotestatnt)  | Non-blocking, SSL, inline keyboards |
| HTTP Downloader | HTTPClient (built-in)        | Stream-based chunked writes to SD  |
| LCD Driver      | TFT_eSPI (Bodmer)            | ST7735 config, fast SPI            |
| JSON            | ArduinoJson v7               | Webhook & config parsing            |
| Settings        | Preferences (built-in)       | NVS key-value store                 |
| mDNS            | ESPmDNS (built-in)           | `usbdrive.local` discovery          |

---

## Development Platform

**PlatformIO (VS Code) with official Espressif platform `espressif32@6.12.0`**

This is the platform used by all working LilyGo T-Dongle-S3 projects (official factory
examples, pjpmarques HelloWorld, JaredReabow 2025). TFT_eSPI works cleanly on this
platform without any SPI register hacks.

Note: USB MSC (Phase 2) feasibility with espressif32@6.12.0 will be assessed when
Phase 1 is hardware-verified. If Core 3.x is required for USB MSC, the Adafruit_ST7735
or Arduino_GFX library will be used instead of TFT_eSPI (they avoid direct SPI register
access and work on both Core 2.x and 3.x).

```ini
[env:t-dongle-s3]
platform = espressif32@6.12.0
board = dongles3
framework = arduino
board_build.partitions = partitions.csv
```

---

## Project File Structure

```
USB Server/
├── CLAUDE.md                 ← this file
├── platformio.ini
├── src/
│   ├── main.cpp              # Entry point, mode manager, FreeRTOS tasks
│   ├── lcd.cpp / lcd.h       # TFT_eSPI wrapper, all screens, progress bar
│   ├── storage.cpp / .h      # SD card mount/unmount, stats, path helpers
│   ├── usb_drive.cpp / .h    # TinyUSB MSC setup and callbacks
│   ├── wifi_manager.cpp / .h # WiFi connect, AP fallback, captive portal, mDNS
│   ├── web_server.cpp / .h   # HTTPS server (fhessel) — all routes on port 443, port 80 redirect
│   ├── downloader.cpp / .h   # HTTPClient + FreeRTOS download queue
│   ├── telegram_bot.cpp / .h # AsyncTelegram2 bot integration
│   ├── config.cpp / .h       # NVS Preferences read/write, defaults
│   └── themes.cpp / .h       # Color palettes, LCD theme apply
└── data/                     # SPIFFS-hosted web UI (uploaded separately)
    ├── index.html
    └── style.css
```

---

## Feature Registry

### Core Features

| Feature                     | Module                | Status   |
|-----------------------------|-----------------------|----------|
| USB Mass Storage (Drive Mode) | `usb_drive`         | [x] Hardware verified |
| HTTPS Config Dashboard      | `web_server`          | [x] Hardware verified |
| HTTPS File Manager          | `web_server`          | [~] Code written — pending hardware verify |
| Direct URL download         | `downloader`          | [ ] Todo |
| LCD UI with progress bar    | `lcd`                 | [x] Done |
| WiFi connect from web/LCD   | `wifi_manager`        | [x] Done |
| Captive portal (first boot) | `wifi_manager`        | [x] Done |
| Theme switching             | `themes` + `lcd`      | [x] Hardware verified |
| Mode switch (USB ↔ Network) | `main`                | [x] Hardware verified |
| USB→Network via bat file    | `main`+`usb_drive`    | [x] Hardware verified (~20 s, Windows FAT32 lazy flush) |
| mDNS (`usbdrive.local`)     | `wifi_manager`        | [x] Done |
| Settings persistence (NVS)  | `config`              | [x] Done |
| Button ≥3s WiFi reset       | `main`                | [x] Hardware verified |

### Remote & Automation Features

| Feature                     | Module                | Status   |
|-----------------------------|-----------------------|----------|
| Telegram Bot (download link)| `telegram_bot`        | [ ] Todo |
| Telegram status replies     | `telegram_bot`        | [ ] Todo |
| Webhook POST `/api/download`| `web_server`          | [ ] Todo |
| Download queue (FreeRTOS)   | `downloader`          | [ ] Todo |
| LCD download queue display  | `lcd`                 | [ ] Todo |
| Auto-pack manifest download | `downloader`          | [ ] Todo |

### Advanced / Polish Features

| Feature                     | Module                | Status   |
|-----------------------------|-----------------------|----------|
| QR code on LCD              | `lcd`                 | [ ] Todo |
| Password protection (web)   | `web_server`+`config` | [ ] Todo |
| Internal temp monitoring    | `main` + `lcd`        | [ ] Todo |
| SD card stats on LCD        | `storage` + `lcd`     | [x] Done |
| Auto-AP fallback            | `wifi_manager`        | [ ] Todo |
| File type icons on LCD      | `lcd`                 | [ ] Todo |
| Download history log        | `downloader`          | [ ] Todo |
| HTTPS web UI (self-signed)  | `web_server`          | [x] Done — fhessel, port 443, port 80 redirect |

---

## Build Phases

### Phase 1 — Foundation `[x]` COMPLETE
- [x] PlatformIO project created, `platformio.ini` configured for T-Dongle-S3
- [x] TFT_eSPI LCD boots with splash screen — portrait 80×160, blink-free 15 s refresh
- [x] APA102 RGB LED working — blue=booting, orange=connecting, green=ok, red=no WiFi
- [x] WiFi captive portal + NVS creds verified on hardware
- [x] SD card mount verified — 29.7 GB free confirmed on hardware

### Phase 2 — Dual Mode Core `[x]` COMPLETE (hardware verified)
- [x] USB MSC active in Drive Mode — SD card visible as removable drive on PC
- [x] Mode switching via BOOT button short press → save to NVS → restart
- [x] LCD reflects active mode: lcd_show_usb_mode() / lcd_show_status()

### Phase 3 — Config Web UI `[~]` Partially hardware verified
- [x] HTTPS config dashboard at `https://[ip]/` (self-signed cert, CN=usbdrive.local) — works in both STA and AP modes
  - Port 443: all routes (fhessel/esp32_https_server)
  - Port 80: HTTP→HTTPS redirect (captive portal NCSI passthrough in AP mode)
  - Status card: mode, WiFi, IP, storage
  - Soft mode switch (no button needed)
  - WiFi settings: scan (refreshes every 30 s, preserves selection) + save + restart
  - IP mode: "Automatic" (DHCP) or "Static IP" (IP/Mask/Gateway fields)
  - Theme selector: Dark / Ocean / Retro (live LCD preview)
  - File manager: list, upload (raw binary), download, delete
- [x] USB→Network mode switch via bat file — `Switch_to_Network_Mode.bat` on SD root
  - Writes `SWITCH_TO_NETWORK` to `/_switch_network.txt` (~20 s, Windows FAT32 lazy flush)
  - Magic bytes: `on_write` detects `SWITCH_TO_NETWORK` string → immediate switch
  - SCSI eject fallback: checks for `/_switch_network.txt` on remount
- [x] Button ≥3s in loop → WiFi reset (raised from 2s to prevent accidental resets)
- [x] Themes: 3 palettes (Dark/Ocean/Retro), NVS-saved, live LCD apply — hardware verified
- [ ] HTTP file manager: list, upload, download, delete files on SD
- [ ] Direct URL download: submit link → LCD progress bar → file on SD
- [ ] Download queue: multiple URLs queued, processed sequentially

### Phase 4 — Remote Control `[ ]`
- [ ] Telegram Bot: `/download <url>` command triggers download, bot replies with status
- [ ] Webhook: `POST /api/download {"url":"..."}` → 200 OK → queued
- [x] Captive portal AP on first boot (no saved WiFi creds) — moved to Phase 1
- [x] Auto-AP fallback if saved WiFi is unreachable — moved to Phase 1
- [x] mDNS: device accessible at `http://usbdrive.local` — moved to Phase 1

### Phase 5 — Polish `[ ]`
- [~] Themes: 3 palettes (moved to Phase 3) — done
- [ ] Auto-pack: `manifest.json` on SD root lists URLs → one-tap sync all
- [ ] QR code on LCD: encodes web UI URL on boot
- [ ] Password protection: HTTPS Basic Auth
- [ ] Temperature sensor: warn on LCD if chip > 75°C, log to SD

---

## Power & Thermal Reference

| State               | Approx. Current | Notes                        |
|---------------------|-----------------|------------------------------|
| Drive Mode (idle)   | ~80 mA          | No WiFi                      |
| Network Mode (idle) | ~120 mA         | WiFi associated, no transfer |
| Network Mode (TX)   | ~270 mA peak    | WiFi transmitting            |
| Sustained Network   | ~150 mA avg     | Typical FTP/download session |

- No hardware thermal shutdown — junction limit 125°C
- Soft warning at 75°C via internal temp sensor
- Avoid sustained high-throughput in enclosed USB hubs

---

## Verification Checklist (End-to-End)

- [ ] Drive Mode: plug into PC → removable drive appears → copy 1 GB file both directions
- [ ] Mode switch: press button → WiFi connects in < 5s → LCD shows IP
- [ ] Web UI: browser to `https://usbdrive.local` (accept self-signed cert) → file list → upload/download/delete
- [ ] URL Download: paste link in web UI → LCD shows progress → file on SD
- [ ] Telegram: send `/download https://example.com/file.zip` → bot confirms → file appears
- [ ] Webhook: `curl -X POST https://usbdrive.local/api/download -H "Content-Type: application/json" -d '{"url":"..."}'` → 200 OK → file queued
- [ ] Theme: change theme in web UI → LCD palette updates immediately
- [ ] Auto-pack: `manifest.json` on SD → tap Sync → all listed URLs download
- [ ] Temp warning: simulate high temp → LCD warning appears

---

## Change Log

| Phase | Date | Change |
|-------|------|--------|
| —     | 2026-03-10 | Initial architecture and feature plan documented |
| 1     | 2026-03-10 | Phase 1 code written: platformio.ini, partitions.csv, config.h, lcd.h/cpp, storage.h/cpp, wifi_manager.h/cpp, main.cpp |
| 1     | 2026-03-10 | Captive portal WiFi setup pulled forward from Phase 4 — NVS credentials, USBDrive-Config AP, BOOT-hold reset |
| 1     | 2026-03-11 | Switched platform from PIOARDUINO to espressif32@6.12.0 (all working T-Dongle-S3 examples use this). Deleted fix_network_h.py (was PIOARDUINO workaround). Fixed lcd_set_backlight() active-LOW inversion bug. |
| 1     | 2026-03-11 | Portrait display: setRotation(2), 80×160 layout (HDR_H=14, FTR_Y=148). Header shows operational mode string. All strings ≤13 chars for GLCD size-1 font on 80 px width. |
| 1     | 2026-03-11 | Fixed RGB LED: chip is APA102 (SPI, DATA=GPIO40, CLK=GPIO39), not WS2812B. FastLED ~3.6.0, BGR color order, bitbang SPI on ESP32-S3. |
| 1     | 2026-03-11 | Blink-free LCD refresh: lcd_show_status() draws header/labels/footer once (static layout_drawn flag); subsequent 15 s refreshes repaint only the two 13 px value rows. |
| 1     | 2026-03-11 | main.cpp cleanup: g_last_refresh promoted to file scope and stamped in setup() so loop() waits a full 15 s before first auto-refresh. |
| 1     | 2026-03-11 | Phase 1 COMPLETE — SD card mount hardware-verified: 29.7 GB detected. All Phase 1 features confirmed working on device. |
| 2     | 2026-03-12 | Phase 2 code written: usb_drive.h/cpp (USBMSC + sdmmc raw sector R/W), ftp_server.h/cpp (SimpleFTPServer wrapper), AppMode enum + NVS_KEY_MODE in config.h, lcd_show_usb_mode() + lcd_invalidate_layout() in lcd, mode-aware main.cpp (load/save mode NVS, restart-on-switch). SimpleFTPServer ^2.1.11 added to platformio.ini. |
| 2     | 2026-03-12 | Phase 2 HARDWARE VERIFIED — USB MSC and mode switching confirmed working on device. |
| 3     | 2026-03-12 | Phase 3 code written: themes.h/cpp (Dark/Ocean/Retro palettes, NVS), lcd_apply_theme() (live palette switch), web_server.h/cpp (config dashboard at :80 — status, soft mode switch, WiFi settings, FTP creds, theme selector), wifi_manager refactored to non-blocking AP mode (WebServer removed, DNS only), ftp_server loads credentials from NVS, FTP+HTTP now start in both STA and AP modes, button ≥2s WiFi reset works in loop (not just at boot). |
| 3     | 2026-03-13 | Web UI improvements: IP mode renamed "DHCP"→"Automatic"; static IP fields reordered (IP/Mask/Gateway), DNS field removed; g_wf dirty flag prevents periodic load() from overwriting in-progress form edits; WiFi scan now refreshes every 30 s with SSID selection preserved. LCD lcd_show_status(): Static IP mode shows labeled IP/Mask/Gateway rows; DHCP shows "Automatic" label. |
| 3     | 2026-03-13 | USB→Network bat file: replaced PowerShell approach with mshta+Shell Eject (flushes FAT32 write cache before SCSI eject). Both magic-bytes (on_write) and SCSI-eject paths supported; magic-bytes handler now removes /_switch_network.txt to prevent revert loop on next USB boot. |
| 3     | 2026-03-13 | Bug fixes (hardware-verified): Fixed revert-to-Network loop after bat-triggered switch. Raised BOOT button WiFi reset threshold 2000→3000 ms to prevent accidental resets. HTTP config dashboard hardware-verified working. USB↔Network mode switching stable and reliable. USB→Network bat takes ~20 s (Windows FAT32 lazy flush — accepted). |
| 3     | 2026-03-14 | Simplified bat file: removed mshta Shell Eject (no timing improvement). Bat now writes magic-bytes file only; relies on on_write detection + SCSI eject fallback. |
| 3     | 2026-03-14 | HTTPS migration: replaced Arduino WebServer with fhessel/esp32_https_server. All routes migrated to port 443 (self-signed RSA-2048 cert, CN=usbdrive.local, 2 yr). Port 80 issues HTTP→HTTPS redirect; AP mode passes NCSI connecttest.txt through. Upload changed to raw binary body + path query param (no multipart). File manager download now works in Chrome (was blocked as insecure HTTP download). Patched HTTPConnection.hpp in library to replace missing hwcrypto/sha.h with inline mbedTLS shim. Build: RAM 22.9%, Flash 30.9%. |
| —     | 2026-03-14 | GitHub repo created: https://github.com/mhmd2520/t-dongle-s3-usb-server. Tagged v0.1.0 as stable baseline. |
