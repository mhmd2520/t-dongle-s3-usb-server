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
│  SD card → PC       │              │  HTTP Web UI + Download  │
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
│  │  SD card → host     │   │  ├─ HTTP Web UI (port 80)      │  │
│  │  (read/write)       │   │  ├─ Webhook API (/api/download) │  │
│  └─────────────────────┘   │  ├─ Telegram Bot (HTTPS)       │  │
│                             │  └─ mDNS (usbdrive.local)     │  │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │               SHARED STORAGE LAYER                        │  │
│  │  SD Card (FAT32) — full 32 GB                            │  │
│  │  NVS Flash — WiFi creds, settings, themes, last mode     │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                     LCD UI LAYER                          │  │
│  │  LovyanGFX — ST7735 80×160 px                            │  │
│  │  Mode status / WiFi info / IP / progress bar / QR code   │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Library Stack

| Purpose         | Library                      | Notes                              |
|-----------------|------------------------------|------------------------------------|
| USB MSC         | `USBMSC.h` (arduino-esp32)   | Built into Arduino Core 3.x        |
| HTTP Server     | mathieucarbou/ESPAsyncWebServer | Port 80, plain HTTP, async, no TLS |
| Telegram Bot    | AsyncTelegram2 (cotestatnt)  | Non-blocking, SSL, inline keyboards |
| HTTP Downloader | HTTPClient (built-in)        | Stream-based chunked writes to SD  |
| LCD Driver      | LovyanGFX (lovyan03)         | ST7735 config, Core 3.x compatible |
| JSON            | ArduinoJson v7               | Webhook & config parsing            |
| Settings        | Preferences (built-in)       | NVS key-value store                 |
| mDNS            | ESPmDNS (built-in)           | `usbdrive.local` discovery          |

---

## Development Platform

**PlatformIO (VS Code) with pioarduino `55.03.37` (Arduino Core 3.3.7, ESP-IDF 5.5.2)**

pioarduino enables a hybrid build mode via `custom_sdkconfig`: lwIP and other IDF components
are compiled from source, allowing TCP window tuning, window scaling, and other settings
that are hard-coded in the prebuilt `liblwip.a` of `espressif32@6.12.0`.

```ini
[env:t-dongle-s3]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.37/platform-espressif32.zip
board = dongles3
framework = arduino
board_build.partitions = partitions.csv
extra_scripts = pre:disable_idf_comp_mgr.py
                pre:add_idf_includes.py
```

**First build**: ~20-40 min (IDF compiled from source). **Subsequent**: ~2-4 min (cached).

---

## Project File Structure

```
USB Server/
├── CLAUDE.md                 ← this file
├── AGENTS.md                 ← multi-agent system roster and usage guide
├── platformio.ini
├── add_idf_includes.py       # SCons pre-script: adds 3 pioarduino prebuilt include dirs
├── disable_idf_comp_mgr.py   # SCons pre-script: disables IDF component manager
├── partitions.csv
├── src/
│   ├── main.cpp              # Entry point, mode manager, button handler
│   ├── lcd.cpp / lcd.h       # LovyanGFX wrapper, all screens, progress bar
│   ├── storage.cpp / .h      # SD card mount/unmount, stats, path helpers
│   ├── usb_drive.cpp / .h    # TinyUSB MSC setup and callbacks
│   ├── usb_stubs.c           # Weak-symbol stubs for Core 3.x USB (pioarduino)
│   ├── wifi_manager.cpp / .h # WiFi connect, AP fallback, captive portal, mDNS
│   ├── web_server.cpp / .h   # HTTP server (ESPAsyncWebServer) — all routes on port 80
│   ├── downloader.cpp / .h   # URL downloader — double-buffer FreeRTOS SD task, TCP window scaling
│   ├── actlog.cpp / .h       # Activity log — 50-entry ring buffer, GET /api/log
│   ├── config.h              # Pin defs, NVS keys, compile-time constants
│   └── themes.cpp / .h       # Color palettes, LCD theme apply
```

---

## Feature Registry

### Core Features

| Feature                     | Module                | Status   |
|-----------------------------|-----------------------|----------|
| USB Mass Storage (Drive Mode) | `usb_drive`         | [x] Hardware verified |
| HTTP Config Dashboard       | `web_server`          | [x] Hardware verified |
| HTTP File Manager           | `web_server`          | [x] Hardware verified |
| Direct URL download         | `downloader`          | [x] Hardware verified — v1.4.0 |
| LCD UI with progress bar    | `lcd`                 | [x] Done |
| WiFi connect from web/LCD   | `wifi_manager`        | [x] Done |
| Captive portal (first boot) | `wifi_manager`        | [x] Done |
| Theme switching             | `themes` + `lcd`      | [x] Hardware verified |
| Mode switch (USB ↔ Network) | `main`                | [x] Hardware verified |
| USB→Network via bat file    | `main`+`usb_drive`    | [x] Hardware verified (~20 s, Windows FAT32 lazy flush) |
| mDNS (`usbdrive.local`)     | `wifi_manager`        | [x] Done |
| Settings persistence (NVS)  | `config`              | [x] Done |
| Button ≥2s WiFi reset       | `main`                | [x] Hardware verified |

### Remote & Automation Features

| Feature                     | Module                | Status   |
|-----------------------------|-----------------------|----------|
| Telegram Bot (download link)| `telegram_bot`        | [ ] Todo |
| Telegram status replies     | `telegram_bot`        | [ ] Todo |
| Webhook POST `/api/download`| `web_server`          | [ ] Todo |
| Download queue (FreeRTOS)   | `downloader`          | [ ] Todo |
| LCD download queue display  | `lcd`                 | [ ] Todo |
| Auto-pack manifest download | `downloader`          | [x] Done — v1.5.0, /_manifest.json, port 8080 |

### Advanced / Polish Features

| Feature                     | Module                | Status   |
|-----------------------------|-----------------------|----------|
| QR code on LCD              | `lcd`                 | [x] Done — v1.5.0, ricmoo/QRCode, 5s on boot |
| Password protection (web)   | `web_server`+`config` | [x] Done — v1.5.0, HTTP Basic Auth, NVS, BOOT-reset |
| Internal temp monitoring    | `main` + `lcd`        | [ ] Removed from scope |
| SD card stats on LCD        | `storage` + `lcd`     | [x] Done |
| Auto-AP fallback            | `wifi_manager`        | [x] Done |
| File type icons on LCD      | `lcd`                 | [ ] Todo |
| Download history log        | `downloader`          | [x] Done — actlog 50-entry ring buffer, GET /api/log |
| HTTP web UI                 | `web_server`          | [x] Done — ESPAsyncWebServer, port 80, plain HTTP |

---

## Build Phases

### Phase 1 — Foundation `[x]` COMPLETE
- [x] PlatformIO project created, `platformio.ini` configured for T-Dongle-S3
- [x] LovyanGFX LCD boots with splash screen — portrait 80×160, blink-free 15 s refresh
- [x] APA102 RGB LED working — blue=booting, orange=connecting, green=ok, red=no WiFi
- [x] WiFi captive portal + NVS creds verified on hardware
- [x] SD card mount verified — 29.7 GB free confirmed on hardware

### Phase 2 — Dual Mode Core `[x]` COMPLETE (hardware verified)
- [x] USB MSC active in Drive Mode — SD card visible as removable drive on PC
- [x] Mode switching via BOOT button short press → save to NVS → restart
- [x] LCD reflects active mode: lcd_show_usb_mode() / lcd_show_status()

### Phase 3 — Config Web UI `[x]` COMPLETE (hardware verified)
- [x] HTTP config dashboard at `http://[ip]/` — works in both STA and AP modes
  - Port 80: all routes (mathieucarbou/ESPAsyncWebServer, fully async, plain HTTP)
  - Captive portal: Android 204 + Windows NCSI + iOS redirect in AP mode
  - Status card: mode, WiFi, IP, storage
  - Soft mode switch (no button needed)
  - WiFi settings: manual-only scan (↻ Refresh button, both AP and STA modes) + save + restart
  - IP mode: "Automatic" (DHCP) or "Static IP" (IP/Mask/Gateway fields)
  - Theme selector: Dark / Ocean / Retro (live LCD preview)
  - File manager: list, upload (raw binary), download, delete, rename, mkdir, ZIP download, search
- [x] USB→Network mode switch via bat file — `Switch_to_Network_Mode.bat` on SD root
  - Writes `SWITCH_TO_NETWORK` to `/_switch_network.txt` (~20 s, Windows FAT32 lazy flush)
  - Magic bytes: `on_write` detects `SWITCH_TO_NETWORK` string → immediate switch
  - SCSI eject fallback: checks for `/_switch_network.txt` on remount
- [x] Button ≥2s in loop → WiFi reset
- [x] Themes: 3 palettes (Dark/Ocean/Retro), NVS-saved, live LCD apply — hardware verified
- [x] File manager: list, upload (multi-file + folder), download, delete, rename, mkdir
- [x] ZIP folder download: two-pass via SD temp file (CRC pre-compute + correct headers)
- [x] File search: recursive depth-limited walk, 200-result cap
- [x] Browser cache fix: `Cache-Control: no-store` on HTML pages
- [x] Concurrent request protection: portMUX spinlock + 503 busy response

### Phase 4 — Remote Control & Downloads `[x]` COMPLETE (hardware verified)
- [x] Direct URL download: submit link via web UI → LCD progress bar → file on SD (hardware verified)
- [x] Download performance: double-buffer FreeRTOS SD write task (Core 1, priority 5) + TCP window scaling (512 KB effective window) — eliminates burst/pause pattern
- [x] LCD "Connecting..." → "Downloading" header transition — real-time feedback before and during download
- [x] URL download stability: submit threshold restored to DL_HALF_SIZE (32 KB) — eliminates mid-download pause/stuck
- [x] Conflict resolution modal: Replace / Keep Both (numbered suffix) / Cancel
- [x] Cancel button: POST /api/dl-cancel stops active download, restores .bak on Replace conflict
- [x] Activity log: GET /api/log — 50-entry ring buffer, logs every download/cancel/skip with size and speed
- [ ] Download queue: multiple URLs queued, processed sequentially (FreeRTOS task)
- [ ] LCD download queue display: progress bar + current filename
- [ ] Telegram Bot: `/download <url>` command triggers download, bot replies with status
- [ ] Webhook: `POST /api/download {"url":"..."}` → 200 OK → queued
- [x] Captive portal AP on first boot (no saved WiFi creds) — moved to Phase 1
- [x] Auto-AP fallback if saved WiFi is unreachable — moved to Phase 1
- [x] mDNS: device accessible at `http://usbdrive.local` — moved to Phase 1

### Phase 5 — Polish `[x]` COMPLETE
- [x] Themes: 3 palettes (moved to Phase 3) — done
- [x] Auto-pack: `/_manifest.json` on SD → "Sync All" button → batch download, skips existing files
- [x] QR code on LCD: encodes web UI URL on boot, 5-second display before status screen
- [x] Password protection: HTTP Basic Auth (NVS-stored, both port 80 + 8080, BOOT button clears)
- [x] Filter system files from file manager listing, search, and ZIP — `is_system_entry()` hides Windows (`System Volume Information`, `$RECYCLE.BIN`, `$*`), macOS (`._*`, `.Trashes`, `.Spotlight-V100`, `.fseventsd`), and internal temp file (`_dl_tmp.zip`)
- [x] ZIP temp file cleanup: post-stream delete with error logging; Range-error early-exit path covered
- [x] Sort WiFi scan results by RSSI (strongest first)
- [x] Track upload write errors: `write_fail_bytes` accumulated, returned in error JSON with `lost_bytes` field

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

- [x] Drive Mode: plug into PC → removable drive appears → copy files both directions
- [x] Mode switch: press button → WiFi connects → LCD shows IP
- [x] Web UI: browser to `http://usbdrive.local` → file list → upload/download/delete
- [x] URL Download: paste link in web UI → LCD shows progress → file on SD
- [ ] Telegram: send `/download https://example.com/file.zip` → bot confirms → file appears
- [ ] Webhook: `curl -X POST http://usbdrive.local/api/download -H "Content-Type: application/json" -d '{"url":"..."}'` → 200 OK → file queued
- [x] Theme: change theme in web UI → LCD palette updates immediately
- [x] Auto-pack: `/_manifest.json` on SD → tap "Sync All" → all listed URLs queued for download
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
| 3     | 2026-03-14 | ZIP CRC fix: replaced custom 16-entry nibble-table CRC32 with `crc32_le()` from `rom/crc.h` (ESP32 ROM, definitively correct). Both CRC-precompute pass and streaming pass now use exact `e.size` byte-count limits — eliminates CRC/size mismatch in ZIP local headers. |
| 3     | 2026-03-14 | SSL partial-write fix: `res_write_all()` retries up to 50× with delay(5) on n==0 (mbedTLS would-block) instead of breaking immediately — prevents silent 306-byte truncated downloads. |
| 3     | 2026-03-14 | ECDSA P-256 cert: replaced RSA-2048 cert (791B+1217B) with ECDSA P-256 (395B+121B) for ~10× faster TLS handshakes. **REVERTED** — fhessel `setupCert()` hardcodes `SSL_CTX_use_RSAPrivateKey_ASN1()` which rejects EC keys silently; server never starts. Restored RSA-2048 cert. |
| 3     | 2026-03-14 | Chrome download fix: `dlBlob()` uses fetch()+URL.createObjectURL(blob) so Chrome never triggers "Check internet connection" block (self-signed HTTPS in AP mode). |
| 3     | 2026-03-14 | Concurrent-request crash fix: `BusyGuard` RAII sets `s_busy` flag during download/zip/upload. Second heavy request returns HTTP 503 `{"error":"busy","op":"..."}` immediately. fhessel confirmed single-threaded cooperative loop (no FreeRTOS tasks per connection) — shared `s_dl_buf`/`s_zip_entries` statics are protected. |
| 3     | 2026-03-14 | Activity indicator: fixed-position toast in file manager shows current op (⬇ Downloading / ⏳ Preparing ZIP / ⬆ Uploading). Back/Forward nav buttons locked (`navLock`) during active fetch to prevent accidental cancellation. 503-busy alert shown if server rejects request. |
| 3     | 2026-03-14 | Per-file download button: explicit ⬇ button added to every file row (rename ✏ / download ⬇ / delete 🗑), in addition to filename-click download link. |
| 3     | 2026-03-14 | API batching: `/api/init` combined endpoint returns status + WiFi scan HTML in one JSON response. Root dashboard first-load uses `/api/init` (one TLS handshake vs two). Periodic refresh keeps `/api/status` and `/api/scan` separate. Build: RAM 22.9%, Flash 31.0%. |
| 3     | 2026-03-14 | Bug fix: ECDSA cert caused server to be completely unreachable. Root cause: `HTTPSServer::setupCert()` in fhessel calls `SSL_CTX_use_RSAPrivateKey_ASN1()` — rejects EC private keys silently, `g_https.start()` returns 0, no socket is bound. Reverted to RSA-2048. |
| 3     | 2026-03-14 | Bug fix: async WiFi scan. `WiFi.scanNetworks()` was called synchronously (1-3 s block) inside request handlers — blocked TLS handshakes → 9/10 connections failed. Fixed: `web_server_loop()` triggers `WiFi.scanNetworks(true)` (async) every 60 s; `get_networks_html()` reads `WiFi.scanComplete()` — never blocks. |
| 3     | 2026-03-14 | Bug fix: FILEMAN_HTML (16592 B) truncated at TLS record boundary (16384 B). ESP-IDF `SSL_write()` sends one TLS record per call; `res->print()` doesn't retry. Fixed `handle_files_html` and `handle_root` to use `res_write_all()` (retry loop). Added forward declaration. |
| 3     | 2026-03-15 | **HTTP migration**: replaced fhessel/esp32_https_server with mathieucarbou/ESPAsyncWebServer (plain HTTP port 80). Removed TLS/mbedTLS stack entirely. Result: instant connections (no RSA handshake), RAM 20.9% (was 22.9%), Flash 23.1% (was 31.0%). Removed patch_https_lib.py + extra_scripts. Removed ssl_certs.h dependency. |
| 3     | 2026-03-15 | **ZIP CRC fix (attempt 4)**: replaced crc32_le() ROM function (calling-convention ambiguity caused wrong CRC-32/BZIP2 variant) with verified pure-software nibble-table CRC-32/ISO-HDLC. Explicit state init (0xFFFFFFFF) and finalize (state ^ 0xFFFFFFFF). Test vector: crc32("123456789") = 0xCBF43926. |
| 3     | 2026-03-15 | **ZIP SD temp file**: replaced PSRAM buffer (HTTP 507 — OPI PSRAM not enabled) with SD temp file approach. ZIP assembled to `/_dl_tmp.zip`, streamed via AsyncWebServer File response. No RAM constraint regardless of ZIP size. |
| 3     | 2026-03-15 | **USB mode revert fix**: added `SD_MMC.end()` + `storage_begin()` remount before `SD_MMC.remove("/_switch_network.txt")` in magic-bytes handler. Without remount, ESP32 FatFS cache doesn't see raw-sector writes from PC; remove() silently failed leaving stale trigger file → every USB Drive boot immediately reverted to Network Mode. |
| 3     | 2026-03-15 | **POST params fix (qparam)**: `req->getParam(key)` without `post=true` only searches URL query string, ignoring `application/x-www-form-urlencoded` body. All POST handlers were returning 400 silently: mode switch, WiFi save, theme, delete, rename, mkdir. Fixed: `qparam()` now checks body params first (`getParam(key,true)`), falls back to query string. |
| 3     | 2026-03-15 | **ZIP 0-byte fix**: `File::seek()` in `FILE_WRITE` mode on ESP32 FatFS does not seek backward — seek-and-patch approach left local headers with size=0/CRC=0, causing 0-byte extracted files. Replaced with two-pass: pass 1 reads each file computing CRC and tracking actual bytes (not `f.size()`); pass 2 writes ZIP with correct headers from the start. Also filters `/_dl_tmp.zip` from zip_collect to prevent self-inclusion. |
| 3     | 2026-03-15 | **Hardening (codebase review)**: button debounce raised from 50 ms to 200 ms (prevents contact-bounce false triggers); search wrapped in busy flag and depth-limited to 20 levels (prevents parallel SD traversal and stack exhaustion); upload rejects Content-Length > 2 GB at first chunk (prevents disk exhaustion). |
| 3     | 2026-03-15 | **ZIP absolute path fix**: `openNextFile().name()` on ESP32 Core 2.x returns basename only (no directory prefix). `zip_collect()` now builds absolute path explicitly as `sdp + '/' + raw`, so pass-1 `SD_MMC.open()` succeeds and extracts real content instead of 0-byte entries. |
| 3     | 2026-03-15 | **WiFi AP accessibility fix**: background `WiFi.scanNetworks(true)` in AP mode caused radio channel-hopping ~25% drop rate. Removed all background scanning. Both AP and STA modes now scan ONLY when user clicks ↻ Refresh (`/api/scan?start=1`). `handle_init` shows "Click Refresh to scan" placeholder until first manual scan. |
| 3     | 2026-03-15 | **Browser cache fix**: `Cache-Control: no-store` added to `handle_root` and `handle_files_html` responses. Prevents browsers caching stale/broken state from previous firmware flashes, which caused AP mode to appear broken on non-incognito browsers. |
| 3     | 2026-03-15 | **Button WiFi reset**: lowered threshold 3000 ms → 2000 ms. LCD message updated to "Hold BOOT 2s". |
| 3     | 2026-03-15 | **New folder toast**: `mkD()` in FILEMAN_HTML shows setBusy/showToast notifications during mkdir, matching download/upload UX consistency. |
| 3     | 2026-03-15 | **Phase 3 COMPLETE** — all core web UI features hardware-verified: config dashboard, file manager, WiFi settings, IP mode, themes, mode switch, USB↔Network bat file, button reset. |
| 5     | 2026-03-15 | **System file filter**: `is_system_entry()` hides OS-generated entries from file listing, search, and ZIP — Windows (`System Volume Information`, `$RECYCLE.BIN`, `$*`), macOS (`._*`, `.Trashes`, `.Spotlight-V100`, `.fseventsd`), and internal temp file (`_dl_tmp.zip`). |
| 4     | 2026-03-19 | **Direct URL download complete**: POST `/api/download`, streaming to SD via HTTPClient, LCD progress bar (partial refresh — speed/size/ETA rows update without full-screen blink), live speed (KB/s)/size/ETA on LCD and web toast, conflict modal (Replace/Skip/Cancel) with .bak backup/restore pattern, cancel button (`POST /api/dl-cancel`), activity log — `actlog.h/cpp` 50-entry ring buffer, `GET /api/log`. On-load check restores poll state after page reload mid-download. `SO_RCVBUF` setsockopt attempted for TCP window expansion but is a no-op against prebuilt `liblwip.a` in espressif32@6.12.0 (TCP_WND=5760 hardcoded); documented in sdkconfig.defaults. Bug fix: added `s_dl_cancel = false` to early-exit paths (SD not ready / SD full) to prevent stale cancel flag poisoning the next download. |
| —     | 2026-03-19 | **Platform migration: pioarduino 55.03.37** (Arduino Core 3.3.7, ESP-IDF 5.5.2). Replaces espressif32@6.12.0. Enables lwIP source rebuild via `custom_sdkconfig`. `add_idf_includes.py` SCons pre-script adds 3 targeted prebuilt include dirs (`arduino_tinyusb/tinyusb/src`, `arduino_tinyusb/include`, `espressif__mdns/include`) to fix `tusb.h`/`mdns.h` missing on clean builds. `disable_idf_comp_mgr.py` prevents IDF component manager conflicts. `src/usb_stubs.c` adds weak-symbol stubs required by Core 3.x USB. `CMakeLists.txt` added for pioarduino cmake integration. Build: RAM 47.5%, Flash 34.3%. |
| —     | 2026-03-19 | **LovyanGFX replaces TFT_eSPI**: `lovyan03/LovyanGFX ^1.2.0` — Core 3.x compatible, no SPI register hacks. ST7735 config migrated. `lcd.cpp` rewritten for LovyanGFX API. |
| —     | 2026-03-19 | **Double-buffer FreeRTOS SD write task**: `sd_writer_task()` runs on Core 1 (priority 5). Two 32 KB ping-pong buffers (`s_buf[65536]`). SDMMC DMA blocks the write task on a semaphore (CPU released), allowing Arduino loop to drain TCP socket concurrently. Overlaps SD write latency with TCP reads — eliminates idle gaps in download stream. |
| —     | 2026-03-19 | **TCP window scaling**: `CONFIG_LWIP_WND_SCALE=y` + `CONFIG_LWIP_TCP_RCV_SCALE=3` in `custom_sdkconfig`. Effective receive window: 65535 × 8 = 512 KB. At 400 KB/s, covers 1.3 s of SD write stalls without server seeing a zero-window pause — eliminates the burst/pause download pattern. |
| —     | 2026-03-19 | **Multi-agent system**: 10 Claude Code skills in `.claude/skills/` — `master`, `orchestrate`, `architect`, `research`, `compare`, `validate`, `implement`, `debug-build`, `report`, `mem-check`. `AGENTS.md` documents roster, standard pipelines, and design principles. Invoked via `/skill-name` in Claude Code terminal. |
| —     | 2026-03-19 | **Tagged v1.4.0** — 1=pioarduino Core 3.x baseline, 4=Phase 4 (direct URL download) complete and hardware verified. |
| —     | 2026-03-25 | **Tagged v1.4.1** — URL download stability (submit-threshold fix, LCD header fix), 7 performance improvements (NVS caching, LCD SPI row caching, post-DL refresh, busy watchdog, temp ZIP cleanup). Phase 4 core download functionality closed. |
| —     | 2026-03-19 | **Claude skills restructured**: `.claude/skills/*.md` flat files converted to `.claude/skills/<name>/SKILL.md` directory format — all 10 skills (`master`, `orchestrate`, `architect`, `research`, `compare`, `validate`, `implement`, `debug-build`, `report`, `mem-check`) now registered as native `/skill-name` slash commands. |
| 3     | 2026-03-19 | **Bat file UX**: message updated to "Device will restart in Network Mode now." (immediate switch); `pause` replaced with `timeout /t 3 /nobreak >nul` — window auto-closes after 3 s. |
| 3     | 2026-03-19 | **File Manager label**: upload-files button and page `<title>` renamed from "Files" to "Upload" for clarity. |
| 4     | 2026-03-19 | **LCD ETA row**: `C_DIM` → `C_WARN` (yellow, `0xFFE0`) for the ETA row in `lcd_show_progress()` — makes remaining-time visible against dark background. |
| 4     | 2026-03-19 | **File download fix**: replaced `dlBlob()` (fetch+blob) with `dlDirect()` (native anchor click) for SD file downloads — was blocking page until full file loaded into RAM, never showing save dialog. `dlBlob()` retained only for ZIP (needs busy toast while ESP32 builds archive). |
| 4     | 2026-03-19 | **"Keep Both" conflict action**: renamed conflict modal "Skip" → "Keep Both". Instead of aborting, finds next free numbered filename (`file_1.ext`, `file_2.ext`, …) up to `_999` and downloads to that path. "Cancel" remains the abort option. |
| 4     | 2026-03-19 | **SD file download stall fix (root cause)**: `handle_download` replaced `AsyncFileResponse` with `AwsResponseFiller` lambda + `shared_ptr<File>`. `AsyncFileResponse::_fillBuffer()` loops on `available()` which returns 0 after the 128 B VFS internal read buffer drains (Core 3.x) even with data remaining on disk, causing download to stall at ~1,222 B. Filler lambda calls `file.read()` directly driven by content-length — no `available()` check, no stall. |
| 3     | 2026-03-19 | **SD file download fix (take 2)**: Reverted `AwsResponseFiller` approach back to `AsyncFileResponse`. Inspection of installed ESPAsyncWebServer 3.6.0 confirmed `AsyncFileResponse::_fillBuffer()` already uses `file.read(data, len)` directly — the `available()` bug was already fixed in this version. `AwsResponseFiller` returning 0 prematurely (e.g., at any read that returns 0) triggers `RESPONSE_END` immediately per the state machine at line 484 of WebResponses.cpp. `AsyncFileResponse` is the simpler, library-tested path with correct lifetime management. |
| 3     | 2026-03-19 | **SD file download fix (take 3 — chunked)**: Switched `handle_download` to `req->beginChunkedResponse()` (`AsyncChunkedResponse`, `_chunked=true`). Root cause of all prior failures: non-chunked state machine at WebResponses.cpp:484 sets `RESPONSE_END` immediately when filler returns 0 — any transient 0-byte SD read truncates the download. Chunked encoding fixes this: returning 0 sends the proper `0\r\n\r\n` terminator and the browser accepts it as complete transfer. Uses `new File(std::move(f))` + custom-deleter `shared_ptr` to avoid File copy-constructor ambiguity. |
| 3     | 2026-03-19 | **SD file download fix (take 4 — AsyncFileResponse)**: Reverted chunked approach. Root cause identified: `ASYNCWEBSERVER_USE_CHUNK_INFLIGHT=1` (default-on in ESPAsyncWebServer) fires a guard `if (!_in_flight_credit \|\| BUFF_SIZE > space()) return 0` for ALL chunked responses on every ACK — with TCP window constrained to ~1221 bytes during first send, guard caused 1213-byte truncation (maxLen = space() - 8 = 1213). Non-chunked `AsyncFileResponse` bypasses the guard entirely for files ≤ 65535 B (`guard only fires when _sentLength > CONFIG_LWIP_TCP_WND_DEFAULT`). Switched to `req->beginResponse(f, name, "application/octet-stream", true)` (File-object constructor, same path as ZIP download). Added `f.setBufferSize(4096)` to enlarge VFS read buffer. Removed `s_dl_buf` static buffer from `handle_download` (still used by ZIP handlers). RAM 49.1%, Flash 34.3%. |
| 3     | 2026-03-19 | **SD file download fix (take 5 — PSRAM pre-read)**: Root cause: `fread()` inside AsyncWebServer `_fillBuffer` callbacks runs in the AsyncTCP service task where SD card I/O consistently fails/returns 0 after the first TCP segment (all 4 prior approaches hit the same failure). Fix: pre-read entire file into PSRAM in the request handler (main-loop task, same context as every other working SD operation), then serve via `memcpy` lambda — no SD I/O in the async callback path. Uses `ps_malloc()` + `std::shared_ptr<uint8_t>(buf, free)` captured in lambda for automatic PSRAM cleanup when response completes. Fallback for files too large for PSRAM: `AsyncCallbackResponse` with `RESPONSE_TRY_AGAIN` on 0-byte reads (avoids premature `RESPONSE_END`). RAM 49.1%, Flash 34.4%. |
| —     | 2026-03-19 | **Firmware version**: `FW_VERSION` bumped from `"0.1.0"` to `"1.4.0"` in `config.h` to match actual release tag. |
| 3     | 2026-03-20 | **SD file download fix (take 8 — vTaskDelay wait loop + Core 0 pin)**: Root cause of "starts fast then slows to zero": `RESPONSE_TRY_AGAIN` returned when fill slot empty — poll timer only retriggers after 500ms, giving effective ~64KB/s (32KB burst ÷ 500ms). Fix: replaced immediate `RESPONSE_TRY_AGAIN` with 10-retry × 5ms `vTaskDelay` loop (50ms max wait). AsyncTCP pinned to Core 0 (`-DCONFIG_ASYNC_TCP_RUNNING_CORE=0`): each yield releases Core 0 while Core 1 (Arduino loop) runs `lfs_service()` to refill slot in ~32ms. Result: stall reduced 500ms→35ms, throughput limited only by WiFi (~500KB/s). Slot size 16KB→32KB (64KB heap during download). Note: Core 1 pin + vTaskDelay would deadlock (same core). |
| 3     | 2026-03-19 | **SD file download fix (take 7 — CHUNK_INFLIGHT disabled + split strategy)**: take 6 PSRAM approach failed at runtime: `CONFIG_SPIRAM=y` in custom_sdkconfig without `board_build.arduino.memory_type = qio_opi` initialized PSRAM at software level but not hardware (wrong OPI pin/timing) → ps_malloc returned address but access caused device panic → ESP32 reset → Chrome "Site wasn't available". `qio_opi` board variant also attempted but triggers pioarduino generator bug (x509_crt_bundle.S content doubled, duplicate symbol assembler error). Fix: added `-DASYNCWEBSERVER_USE_CHUNK_INFLIGHT=0` to build_flags — disables the in-flight credit guard in ESPAsyncWebServer that was throttling all non-chunked responses after 5760 bytes. Split strategy: files ≤ 64 KB pre-read into SRAM in request handler (main task, zero SD I/O in async callback); files > 64 KB use `AsyncFileResponse` (non-chunked, Content-Length known, guard disabled → write_send_buffs runs to completion on each ACK without artificial back-pressure). SPIRAM sdkconfig settings reverted. Build: RAM TBD, Flash TBD. |
| 3     | 2026-03-20 | **SD file download fix (take 10 — synchronous WebServer port 8080)**: Root cause of all prior failures conclusively: `AsyncFileResponse::_fillBuffer()` and async filler callbacks run in `async_service_task`; SD_MMC DMA reads return 0 bytes intermittently in that task on IDF 5.x (task-affinity DMA change vs IDF 4.x). All ring-buffer, PSRAM, and Core-pin workarounds still hit the same 0-byte reads in the async callback path. Fix: add `WebServer s_dl_srv(8080)` in new `dl_server.cpp` (separate translation unit — avoids `HTTP_GET` name collision between WebServer.h/http_parser.h and ESPAsyncWebServer). `WebServer::streamFile()` called from `dl_server_loop()` inside Arduino `loop()` (Core 1, main task) where SD reads always work. Port 80 keeps small-file (≤64KB) SRAM pre-read path; all large file and ZIP downloads redirect to port 8080. `handle_download_zip` now returns `{"ok":true,"path":"/_dl_tmp.zip","name":"..."}` JSON; JS navigates to port 8080 for the actual file. `dlDirect()` call sites updated to `http://hostname:8080/dl?path=...`; `dlBlob()` updated to use JSON → port 8080 flow. Dead-code lfs_ ring buffer removed. RAM 49.2%, Flash 35.0%. |
| 3     | 2026-03-20 | **ZIP HTTP 501 fix — polling model**: Root cause: ESPAsyncWebServer auto-sends HTTP 501 if a handler returns without calling `req->send()`. Prior deferred approach stored `AsyncWebServerRequest*` and called `req->send()` from `loop()` — framework had already freed the request, causing 501 (and potential dangling-pointer crash). Fix: `handle_download_zip` now sends `{"status":"building"}` immediately and returns. `run_deferred_zip()` (loop task) sets `g_zip_state` / `g_zip_done_json`. New `/api/zip-status` endpoint polled by JS every 500 ms until `{"ok":true}` — then JS navigates anchor to `http://host:8080/dl?path=/_dl_tmp.zip`. `g_zip_client_gone` and `g_zip_req_ptr` removed entirely. Build: RAM 51.7%, Flash 35.0%. |
| 3     | 2026-03-20 | **ZIP white page fix — deferred loop-task execution**: Root cause: `handle_download_zip` ran fully in `async_service_task` (ESPAsyncWebServer callback thread), doing heavy SD I/O (two-pass read + write) for seconds. While blocked, no other port-80 requests could be processed → browser page reload during ZIP build got no response → blank white page. Fix: `handle_download_zip` now only queues the request (`g_zip_requested = true`, stores path + `AsyncWebServerRequest*`, registers `req->onDisconnect` cancel flag) and returns immediately. `run_deferred_zip()` executes the actual build in `web_server_loop()` (Core 1, Arduino loop task) where `async_service_task` remains free. Cancel-aware: checks `g_zip_client_gone` between phases and inside the pass-2 file loop. Build: RAM 51.7%, Flash 35.0%. |
| 3     | 2026-03-20 | **Download speed improvement — `setBufferSize(16384)`**: Added `f.setBufferSize(16384)` to `dl_server.cpp` before `WebServer::streamFile()`, and to every file opened in `zip_pass1_crc()` and `run_deferred_zip()` pass-2. Default VFS buffer is 512 bytes (1 FAT sector); 16 KB buffer reduces SD bus transactions ~32× for sequential reads. `s_dl_buf` enlarged 8 KB → 16 KB for the same reason. `handle_download` large-file path changed from 400 error to HTTP 302 redirect to `http://host:8080/dl?path=...`. |
| 3     | 2026-03-20 | **Web server hang fix — no SD I/O in async_service_task**: Root cause of ERR_CONNECTION_TIMED_OUT / laggy web UI: `handle_download` (port 80) pre-read files (≤ 64 KB) into SRAM via `fread()` DMA inside `async_service_task`. On IDF 5.x this call intermittently blocks/hangs (task-affinity DMA issue), permanently stalling `async_service_task` → port 80 becomes completely unresponsive. Fix: `handle_download` now always redirects to `http://host:8080/dl?path=...` — zero SD I/O in async_service_task. Build: RAM 56.7%, Flash 34.9%. |
| 3     | 2026-03-20 | **WiFi scan sort by RSSI**: `get_networks_html()` now builds a sorted index vector with `std::sort` before rendering `<option>` elements — strongest signal (least negative dBm) listed first. No functional change; improves UX when multiple networks are visible. Build: RAM 56.8%, Flash 35.0%. |
| 3     | 2026-03-20 | **Silent switch script — .vbs replaces .bat**: `Switch_to_Network_Mode.bat` replaced with `Switch_to_Network_Mode.vbs`. VBScript runs via `wscript.exe` (no console window). Uses `FileSystemObject` to write `SWITCH_TO_NETWORK` to `_switch_network.txt` on the SD root drive letter. Old `.bat` file is removed from SD on next USB Drive Mode boot. LCD and web UI hint updated from `.bat` to `.vbs`. Build: RAM 56.7%, Flash 34.9%. |
| 3     | 2026-03-20 | **File Manager loading fix — deferred directory listing**: Root cause: ALL SD operations in `async_service_task` (including POSIX `opendir`/`readdir`/`stat`) are unsafe on IDF 5.x due to SD_MMC DMA task-affinity — they intermittently block/hang, stalling the entire port-80 web server. Fix: `handle_list` now responds immediately with `{"status":"building"}` and sets `g_list_requested`; `run_deferred_list()` executes in `web_server_loop()` (Core 1, main task, safe for SD DMA). Directories sorted: dirs first, then alphabetical (`strcasecmp`). JS `loadDir()` polls `/api/list-status` every 200 ms until done. `g_list_done_json` returned and freed on first poll. Build: RAM 56.8%, Flash 35.0%. |
| 3     | 2026-03-21 | **File Manager fix — port 8080 directory listing and search**: Root cause: deferred+polling approach failed because the initial `fetch('/api/list')` itself hangs when `async_service_task` is saturated (IDF 5.x SD_MMC DMA task-affinity). Both `/list` and `/search` moved to port 8080 synchronous WebServer (runs in `dl_server_loop()` → Arduino `loop()` → Core 1, where SD DMA is always safe). `loadDir()` in FILEMAN_HTML now makes a single `fetch('http://host:8080/list?path=...')` — no deferred mechanism, no polling. Search uses `fetch('http://host:8080/search?q=...')`. All deferred list globals, `run_deferred_list()`, `handle_list()`, `handle_list_status()`, `search_walk()`, `handle_search()` removed from `web_server.cpp`. New `handle_list()` and `handle_search()` in `dl_server.cpp` with `Access-Control-Allow-Origin: *` header (cross-origin fetch from port 80). "Loading…" text changed to `#aaa` (was `#555`) for better visibility on dark background. Build: RAM 56.7%, Flash 35.1%. |
| 3     | 2026-03-20 | **File Manager lagginess fix — POSIX stat() for file sizes**: Root cause: `handle_list` called `entry.size()` (Arduino `VFSFileImpl::size()`) for every file. On IDF 5.x FatFS, `size()` calls `fseek(SEEK_END)` which traverses the entire FAT cluster chain — O(file_size/cluster_size) per file. A directory containing GB-sized files blocked `async_service_task` for seconds, making port 80 unresponsive. Fix: replaced `SD_MMC.open()/openNextFile()/entry.size()` with POSIX `opendir()`/`readdir()`/`stat()` using VFS path (`/sdcard` + SD path). `stat()` reads size directly from the FAT directory entry in O(1). Build: RAM 56.7%, Flash 34.9%. |
| 3     | 2026-03-20 | **Port 8080 download fix — manual send loop**: Root cause: `NetworkClient::write(Stream&)` reads SD in 1360 B chunks; if `write(buf, 1360)` returns 0 (EAGAIN exhausted), that chunk is silently discarded and the loop continues, corrupting/truncating the response. Fix: replaced `streamFile()` in `dl_server.cpp` with a manual loop using a 16 KB buffer and `client().write(buf, n)` — `write(buf, n)` handles partial sends internally and fails loudly (returns 0) on error; we `break` cleanly rather than silently losing data. Also added `dl_server_loop()` call between files in `run_deferred_zip()` so ZIP builds don't starve port 8080 connections. Build: RAM 56.7%, Flash 35.0%. |
| 4     | 2026-03-25 | **LCD "Connecting..." header fix**: Added `s_prog_last_hdr[20]` cache to `lcd_show_progress()`. Header is compared by string on every partial-refresh call — transition "Connecting..." → "Downloading" now triggers an immediate header redraw even when label/pct/queue are unchanged. Previously the header only redrew on `first_draw` or `queue_remaining` change, leaving "Connecting..." stuck on screen for the entire download. `lcd_invalidate_layout()` resets the cache. |
| 4     | 2026-03-25 | **URL download stuck/pause fix**: Removed `DL_SUBMIT_THRESH = 4096` and `timed_flush` introduced in a prior session. These caused the download loop to block on `xSemaphoreTake(s_wr_done)` every ~10 ms (at 4 KB per submit, 400 KB/s) instead of every ~80 ms (at 32 KB per submit). The frequent blocking prevented the TCP drain loop from running, starving the lwIP receive buffer and causing mid-download pauses. Submit condition restored to `rd_fill >= DL_HALF_SIZE` (32 KB) — 8× fewer blocking waits. |
| 4     | 2026-03-25 | **LCD "Connecting..." shown before http.GET()**: Added `lcd_show_progress(..., "Connecting...")` call and `s_dl_status = "connecting"` before `http.GET()`. DNS resolution + TCP connect can take 0.5–3 s; previously the LCD showed nothing during this phase. Second call after GET returns clears "Connecting..." immediately (using header cache fix above). |
| perf  | 2026-03-25 | **Post-download LCD refresh**: `main.cpp` now detects the `busy → idle` transition and calls `refresh_status()` immediately when a download ends, instead of waiting up to 15 s for the periodic timer. |
| perf  | 2026-03-25 | **Busy-flag watchdog 30 s → 15 s**: `web_server_loop()` busy watchdog timeout halved — stalled upload/ZIP operations unlock the server 15 s earlier on dropped connections. |
| perf  | 2026-03-25 | **ZIP status poll 500 ms → 1000 ms**: JS ZIP build polling interval doubled (max 60 iterations unchanged at 60 s). Halves HTTP requests during ZIP assembly with no UX impact. |
| perf  | 2026-03-25 | **Stale temp ZIP cleanup on boot**: `dl_server_begin()` now calls `SD_MMC.remove("/_dl_tmp.zip")` before writing the file manager HTML. Removes any `/_dl_tmp.zip` left by a previous session that was interrupted mid-download. |
| perf  | 2026-03-25 | **NVS ip_mode cache in `lcd_show_status()`**: `ip_mode`/`s_mask`/`s_gw` are now read from NVS only on the first call after boot (when `s_last_ip_mode == 255`). Subsequent 15 s refresh ticks use the cached values — eliminates 2 NVS flash reads per minute during idle operation. WiFi save always restarts, so the cache is inherently invalidated. |
| perf  | 2026-03-25 | **NVS cache in `fill_status_json()`**: `mode`, `ip_mode`, `s_ip`, `s_gw`, `s_mask` are read from NVS once on first `/api/status` or `/api/init` call, then served from static RAM. Two NVS open/close cycles per API call reduced to zero after first boot. |
| perf  | 2026-03-25 | **LCD progress bar SPI row caching**: `lcd_show_progress()` now caches the formatted string for each of the 3 stats rows (size, speed, ETA). A row is only erased and redrawn when its string representation actually changes. Reduces SPI transactions by up to 66% per LCD update tick during downloads (ETA changes every second; size changes far less often). |
| 3     | 2026-03-19 | **SD file download fix (take 6 — OPI PSRAM enabled)**: Root cause of all 5 prior failures confirmed: `ps_malloc()` always returned NULL because PSRAM was disabled (`CONFIG_SPIRAM not set` — dongles3 "No PSRAM variant" default). Take 5 PSRAM path never executed; always fell to `RESPONSE_TRY_AGAIN` fallback which loops forever when SD reads fail in `async_service_task` context. Fix: added `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M=y`, `CONFIG_SPIRAM_BOOT_INIT=y`, `CONFIG_SPIRAM_USE_CAPS_ALLOC=y` to `custom_sdkconfig` in `platformio.ini` — enables 8 MB OPI PSRAM on ESP32-S3R8V at boot. `ps_malloc(fileSize)` now succeeds; `handle_download` pre-reads file into PSRAM in main task, serves via `memcpy` lambda (zero SD I/O in async callback). SRAM fallback (`malloc`) added for files ≤ 64 KB as belt-and-suspenders. Broken `RESPONSE_TRY_AGAIN` fallback replaced with clean HTTP 507 for files too large for memory. Note: `board_build.arduino.memory_type = qio_opi` was tried but caused x509_crt_bundle.S duplicate-symbol assembler failure (pioarduino generator bug with that board variant); reverted — sdkconfig SPIRAM settings alone are sufficient for pioarduino hybrid builds. RAM 49.2%, Flash 34.5%. |
| 5     | 2026-03-28 | **Upload write error tracking**: Added `write_fail_bytes` field to `UploadCtx`. `handle_upload_body()` accumulates `(len-w)` on every partial write. Completion handler returns `{"ok":false,"error":"write failed","lost_bytes":N}` with byte-accurate failure count. Activity log records "Write err N B lost @ K KB". Previously write failures silently produced a corrupt file with a success response. |
| 5     | 2026-03-28 | **ZIP temp cleanup hardening**: `handle_dl()` in `dl_server.cpp` — added error log (`Serial.println`) if `SD_MMC.remove(ZIP_TMP_PATH)` returns false; added cleanup in the Range-Not-Satisfiable early-exit path (line 240) which previously skipped cleanup when the ZIP was served with a Range request. |
| 5     | 2026-03-28 | **Password protection (HTTP Basic Auth)**: New `auth_check()` / `auth_cache_invalidate()` helpers in `web_server.cpp` read NVS keys `auth_u`/`auth_p` once and cache them. All 22 port-80 handlers except `handle_not_found` (captive portal) and HTTP OPTIONS (CORS) guard with `if (!auth_check(req)) return`. All 4 port-8080 handlers in `dl_server.cpp` guard with `dl_auth_check()`. New `POST /api/auth` endpoint saves/clears credentials. "Access Control" card added to dashboard. `wifi_reset_credentials()` also clears auth NVS keys — BOOT button 2s is the recovery path. Empty credentials = open access (backwards compatible). |
| 5     | 2026-03-28 | **QR code on LCD at boot**: `ricmoo/QRCode ^0.0.1` added to `lib_deps`. New `lcd_show_qr(url)` in `lcd.cpp`: encodes URL via `qrcode_initText()` (Version 2/ECC_LOW for URLs ≤32 chars; Version 3 fallback). Renders on white canvas (80px wide, theme-agnostic). Version 2 → 3 px/module = 75px; Version 3 → 2 px/module = 58px. Caption "Scan to open" + truncated URL below QR. In `main.cpp`, called for 5 s after WiFi connects (both STA and AP modes), then `lcd_invalidate_layout()` + normal status screen. |
| 5     | 2026-03-28 | **Auto-pack manifest download**: New `POST /manifest-sync` on port 8080 (`dl_server.cpp`, Core 1). Reads `/_manifest.json` (≤4096 bytes) from SD, parses `{"files":[{"url":"...","path":"..."},...]}` with ArduinoJson. Checks `SD_MMC.exists()` per entry — skips existing files. Calls `downloader_queue()` for new entries. Returns `{"ok":true,"queued":N,"skipped":M,"queue_full":K}`. "Auto-Pack Sync" card added to dashboard with "Sync All" button and manifest format hint. |
| —     | 2026-03-28 | **Tagged v1.5.0** — Phase 5 (Polish) complete: upload error tracking, ZIP cleanup, password protection, QR code on LCD, auto-pack manifest sync. |
