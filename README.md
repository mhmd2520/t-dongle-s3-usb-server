# T-Dongle-S3 Smart USB Drive

> **Work in progress** — full documentation, installation guide, and pre-built firmware will be added when all phases are complete.

A smart USB dongle based on the **LilyGo T-Dongle-S3** (ESP32-S3) that works as both a USB mass storage drive and a wireless file server — switchable on the fly.

## Current Status

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Foundation — LCD, WiFi, APA102 LED, SD card | ✅ Complete |
| 2 | Dual-mode — USB MSC ↔ Network switch | ✅ Complete |
| 3 | Config Web UI — dashboard, file manager, themes | 🔄 In progress |
| 4 | Remote control — Telegram bot, webhook | ⏳ Pending |
| 5 | Polish — HTTPS, QR code, auto-pack, temp sensor | ⏳ Pending |

## Hardware

- **MCU:** ESP32-S3 (LilyGo T-Dongle-S3)
- **Storage:** microSD up to 32 GB
- **Display:** ST7735 0.96" IPS 80×160
- **LED:** APA102 RGB
- **USB:** USB-A device mode

## Build

Built with [PlatformIO](https://platformio.org/) — open `d:/Arduino/USB Server/` in VSCode with the PlatformIO extension installed, then click **Build**.

See `CLAUDE.md` for full architecture, feature registry, and change log.
