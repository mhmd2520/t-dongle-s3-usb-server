# T-Dongle-S3 Smart USB Drive

A smart USB dongle based on the **LilyGo T-Dongle-S3** (ESP32-S3) that works as both a USB mass storage drive and a wireless file server — switchable on the fly.

## Current Status — v0.3.0

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Foundation — LCD, WiFi, APA102 LED, SD card | ✅ Complete |
| 2 | Dual-mode — USB MSC ↔ Network switch | ✅ Complete |
| 3 | Config Web UI — dashboard, file manager, themes | ✅ Complete |
| 4 | Remote control — URL download, Telegram bot, webhook | ⏳ Pending |
| 5 | Polish — QR code, auto-pack, temp sensor, password | ⏳ Pending |

## Features

### USB Drive Mode
- SD card appears as a removable USB drive on any PC
- Switch to Network Mode by double-clicking `Switch_to_Network_Mode.bat` on the drive (no button needed)

### Network Mode
- **Web dashboard** at `http://[device-ip]/` (or `http://usbdrive.local/` on supported networks)
- **File manager** — browse, upload (multi-file + folder), download, delete, rename, create folders, ZIP download, search
- **WiFi settings** — scan networks, DHCP or Static IP
- **Theme selector** — Dark / Ocean / Retro (updates LCD in real time)
- **Soft mode switch** — switch USB ↔ Network from the browser
- **Captive portal** — on first boot, device starts `USBDrive-Config` AP for WiFi setup

### Hardware UI
- ST7735 0.96" LCD — shows mode, IP, storage stats
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

## Build & Flash

Requires [PlatformIO](https://platformio.org/) with the VSCode extension.

```bash
# Build
pio run

# Flash
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
| HTTP Server | mathieucarbou/ESPAsyncWebServer @ ^3.3.23 |
| LCD | bodmer/TFT_eSPI @ ^2.5.43 |
| JSON | bblanchon/ArduinoJson @ ^7.3.1 |
| RGB LED | fastled/FastLED @ ~3.6.0 |

See `CLAUDE.md` for full architecture, feature registry, and change log.
