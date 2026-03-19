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
| Auto-pack manifest download | `downloader`          | [ ] Todo |

### Advanced / Polish Features

| Feature                     | Module                | Status   |
|-----------------------------|-----------------------|----------|
| QR code on LCD              | `lcd`                 | [ ] Todo |
| Password protection (web)   | `web_server`+`config` | [ ] Todo |
| Internal temp monitoring    | `main` + `lcd`        | [ ] Todo |
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

### Phase 4 — Remote Control & Downloads `[~]` (partial)
- [x] Direct URL download: submit link via web UI → LCD progress bar → file on SD (hardware verified)
- [x] Download performance: double-buffer FreeRTOS SD write task (Core 1, priority 5) + TCP window scaling (512 KB effective window) — eliminates burst/pause pattern
- [ ] Download queue: multiple URLs queued, processed sequentially (FreeRTOS task)
- [ ] LCD download queue display: progress bar + current filename
- [ ] Telegram Bot: `/download <url>` command triggers download, bot replies with status
- [ ] Webhook: `POST /api/download {"url":"..."}` → 200 OK → queued
- [x] Captive portal AP on first boot (no saved WiFi creds) — moved to Phase 1
- [x] Auto-AP fallback if saved WiFi is unreachable — moved to Phase 1
- [x] mDNS: device accessible at `http://usbdrive.local` — moved to Phase 1

### Phase 5 — Polish `[ ]`
- [x] Themes: 3 palettes (moved to Phase 3) — done
- [ ] Auto-pack: `manifest.json` on SD root lists URLs → one-tap sync all
- [ ] QR code on LCD: encodes web UI URL on boot
- [ ] Password protection: HTTP Basic Auth
- [ ] Temperature sensor: warn on LCD if chip > 75°C, log to SD
- [x] Filter system files from file manager listing, search, and ZIP — `is_system_entry()` hides Windows (`System Volume Information`, `$RECYCLE.BIN`, `$*`), macOS (`._*`, `.Trashes`, `.Spotlight-V100`, `.fseventsd`), and internal temp file (`_dl_tmp.zip`)
- [ ] ZIP temp file cleanup: delete `/_dl_tmp.zip` after streaming completes
- [ ] Sort WiFi scan results by RSSI (strongest first)
- [ ] Track upload write errors: check `file.write()` return per chunk

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
| —     | 2026-03-19 | **Claude skills restructured**: `.claude/skills/*.md` flat files converted to `.claude/skills/<name>/SKILL.md` directory format — all 10 skills (`master`, `orchestrate`, `architect`, `research`, `compare`, `validate`, `implement`, `debug-build`, `report`, `mem-check`) now registered as native `/skill-name` slash commands. |
| 3     | 2026-03-19 | **Bat file UX**: message updated to "Device will restart in Network Mode now." (immediate switch); `pause` replaced with `timeout /t 3 /nobreak >nul` — window auto-closes after 3 s. |
| 3     | 2026-03-19 | **File Manager label**: upload-files button and page `<title>` renamed from "Files" to "Upload" for clarity. |
| 4     | 2026-03-19 | **LCD ETA row**: `C_DIM` → `C_WARN` (yellow, `0xFFE0`) for the ETA row in `lcd_show_progress()` — makes remaining-time visible against dark background. |
