# T-Dongle-S3 Smart USB Drive

A smart USB dongle based on the **LilyGo T-Dongle-S3** (ESP32-S3) that works as both a USB mass storage drive and a wireless file server — switchable on the fly.

## Current Status — v1.4.1

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Foundation — LCD, WiFi, APA102 LED, SD card | ✅ Complete |
| 2 | Dual-mode — USB MSC ↔ Network switch | ✅ Complete |
| 3 | Config Web UI — dashboard, file manager, themes | ✅ Complete |
| 4 | URL download — web UI trigger, LCD progress, conflict resolution | ✅ Complete |
| 5 | Polish — QR code, auto-pack, temp sensor, password | ⏳ Pending |

## Features

### USB Drive Mode
- SD card appears as a removable USB drive on any PC
- Switch to Network Mode by double-clicking `Switch_to_Network_Mode.vbs` on the drive (no button needed)
- Or hold the BOOT button for 2 s to reset WiFi and restart in Network Mode

### Network Mode
- **Web dashboard** at `http://[device-ip]/` (or `http://usbdrive.local/` via mDNS)
- **File manager** — browse, upload (multi-file + folder), download, delete, rename, create folders, ZIP download, search
- **URL downloader** — paste any HTTP/HTTPS link → file downloads directly to SD; LCD shows live progress (speed, size, ETA); conflict modal: Replace / Keep Both / Cancel
- **Activity log** — last 50 download events at `GET /api/log`
- **WiFi settings** — scan networks, DHCP or Static IP
- **Theme selector** — Dark / Ocean / Retro (updates LCD in real time)
- **Soft mode switch** — switch USB ↔ Network from the browser
- **Captive portal** — on first boot, device starts `USBDrive-Config` AP for WiFi setup

### Hardware UI
- ST7735 0.96" LCD — mode, IP, storage stats; progress bar during downloads
- APA102 RGB LED — blue=booting, orange=connecting, green=connected, red=error
- BOOT button: short press = switch mode, hold 2 s = reset WiFi credentials

## Hardware

| Component | Spec |
|-----------|------|
| MCU | ESP32-S3 LX7 dual-core @ 240 MHz |
| RAM | 8 MB PSRAM + 512 KB SRAM |
| Flash | 16 MB |
| Storage | microSD (FAT32, up to 32 GB) |
| Display | ST7735 0.96" IPS, 80×160 px |
| LED | APA102 RGB (DATA=GPIO40, CLK=GPIO39) |
| USB | USB-A, device mode only |

## Architecture

Two mutually exclusive modes share the same SD card:

```
USB DRIVE MODE          NETWORK MODE
TinyUSB MSC active  ←→  WiFi + HTTP active
SD card → PC            Web UI + URL downloader
WiFi disabled           USB MSC disabled
```

Switching tears down one stack and initialises the other. Mode is saved to NVS and restored on next boot.

## Build & Flash

Requires [PlatformIO](https://platformio.org/) with the VSCode extension and the **pioarduino** platform (ESP-IDF 5.5.2, Arduino Core 3.3.7).

> First build compiles ESP-IDF from source — allow 20–40 min. Subsequent builds: ~2–4 min (cached).

```bash
# Build
pio run

# Flash (hold BOOT then press RST to enter download mode first)
pio run -t upload

# Serial monitor
pio device monitor
```

On first boot with no saved WiFi credentials the device starts an AP:
- **SSID:** `USBDrive-Config`
- **Password:** `12345678`
- **URL:** `http://192.168.4.1/`

## Library Stack

| Purpose | Library |
|---------|---------|
| USB MSC | `USBMSC.h` (arduino-esp32 built-in) |
| HTTP Server | mathieucarbou/ESPAsyncWebServer @ ^3.3.23 |
| Sync Download Server | Arduino `WebServer` (port 8080, built-in) |
| HTTP Downloader | `HTTPClient` (built-in) |
| LCD Driver | lovyan03/LovyanGFX @ ^1.2.0 |
| JSON | bblanchon/ArduinoJson @ ^7.3.1 |
| RGB LED | fastled/FastLED @ ~3.6.0 |

## Key Design Notes

- **SD I/O from `async_service_task` (Core 0) fails on IDF 5.x** — all SD reads run on Core 1 via port 8080 (`dl_server.cpp`, synchronous `WebServer`). Port 80 (ESPAsyncWebServer) handles only metadata and small in-memory responses.
- **Double-buffer FreeRTOS SD write task** — TCP reads and SD writes overlap on separate cores; effective download throughput ~400 KB/s over WiFi.
- **TCP window scaling** — 512 KB effective receive window via `CONFIG_LWIP_WND_SCALE=y` + `CONFIG_LWIP_TCP_RCV_SCALE=3` in `custom_sdkconfig`.

See `CLAUDE.md` for full architecture, feature registry, and detailed change log.
