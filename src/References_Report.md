# T-Dongle-S3 Ecosystem — Reference Projects Report

> Researched: 2026-03-22
> Purpose: Survey the T-Dongle-S3 / ESP32-S3 open-source ecosystem for architecture patterns,
> library choices, and design ideas relevant to the T-Dongle-S3 Smart USB Drive project
> (WiFi + USB MSC + HTTP file manager + URL downloader + web UI).

---

## Summary Table

| # | Project | Author | Stars | Primary Use Case | USB Mode | WiFi | Web UI | SD Card | Relevance |
|---|---------|--------|-------|-----------------|----------|------|--------|---------|-----------|
| 1 | T-Dongle-S3 (official) | Xinyuan-LilyGO | ~350 | Hardware reference / examples | HID + MSC | Yes | No | Yes | High — canonical pin map & example sketches |
| 2 | USB Army Knife | i-am-shodan | High | Multi-mode pen-test platform | HID + MSC + NCM + Serial | Yes (AP) | Bootstrap @ :8080 | Yes | High — architecture patterns, multi-mode USB, web UI |
| 3 | T-Dongle-Ducky | LockManipulator | Low | WiFi-controlled rubber ducky | HID keyboard | AP | Custom auth | No | Medium — web-based script control pattern |
| 4 | ZeroTrace | zerotrace.pw | — | Commercial HID automation tool | HID | Yes | Cloud dashboard | — | Low-Medium — commercial feature set reference |
| 5 | PS5-Dongle | stooged | Medium | PS5 exploit delivery | MSC | AP (HTTPS) | Admin panel | Yes | High — dual MSC+web pattern, SD-based payload update |
| 6 | ds-scripts-USB-Army-Knife | Nikorasu-07 | Low | DuckyScript payloads for UAK | HID | — | — | — | Low — script collection only |
| 7 | PS4-Dongle | stooged | Medium | PS4 exploit delivery | MSC | AP (HTTP/S) | Admin panel | Yes | High — same dual MSC+web pattern as PS5-Dongle |
| 8 | pwnstick-lua | Rilshrink | Low | Lua-scripted rubber ducky | HID keyboard+mouse | AP | Auth @ 192.168.1.1 | Yes | Medium — Lua scripting + embedded file manager |
| 9 | T-Dongle-S3-Firmware | kaliwinki-1 | Low | Firmware binary archive | HID + MSC | Yes | Various | — | Low — aggregator, not original code |
| 10 | Bruce v1.0 for T-Dongle-S3 | Mecres256 | Low | Multi-tool GUI overlay | HID + WiFi attacks | Yes | :8080 | Yes | Medium — menu system, PNG-based UI pattern |
| 11 | T-Dongle-S3-forecaster | samsan | Low | Weather display | None | STA | None | No | Low — WiFi API fetch + display pattern |

---

## Project 1 — T-Dongle-S3 (Official LilyGO Repository)

**URL:** https://github.com/Xinyuan-LilyGO/T-Dongle-S3

### What It Is
The official hardware reference repository from LilyGO for the T-Dongle-S3 family of boards.
Contains board definitions, schematics, datasheets, and a rich collection of working example
sketches covering every peripheral on the device.

### Hardware Targeted
- T-Dongle-S3 (base — 16 MB flash, no PSRAM)
- T-Dongle-S3-Dual
- T-Dongle-S3-Plus

### Key Features
17 official example sketches:

| Example | Purpose |
|---------|---------|
| `TFT_eSPI` | ST7735 display with TFT_eSPI library |
| `factory_no_screen` | Factory firmware without display |
| `factory_screen` | Factory firmware with display |
| `fs_webserver` | Filesystem-based web server |
| `ir_send` | Infrared transmission |
| `lcd` | LCD display control |
| `led` | APA102 LED control |
| `lvgl9` | LVGL v9 GUI framework |
| `microphone` | Audio input |
| `micropython` | MicroPython interpreter |
| `qwiic_i2c_scan` | I2C device scanning |
| `qwiic_uart_loopback` | UART loopback test |
| `sd_card` | SD card storage |
| `usb_hid_keyboard` | USB HID keyboard emulation |
| `usb_hid_mouse` | USB HID mouse emulation |
| `usb_mass_storage` | USB MSC device |
| `usb_mass_storage_led` | USB MSC with LED feedback |

### Board Configuration (from `dongles3.json`)

| Parameter | Value |
|-----------|-------|
| MCU | ESP32-S3 |
| CPU frequency | 240 MHz |
| Flash | 16 MB QIO |
| Flash frequency | 80 MHz |
| RAM | 320 KB (no PSRAM variant) |
| USB mode | `ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1` |
| Arduino running core | Core 1 |
| Event running core | Core 1 |
| Upload speed | 921,600 baud |
| USB IDs | 0x303A / 0x82C1 |

### Libraries Used
- TFT_eSPI (display, some examples — version ≤ 2.0.14)
- LovyanGFX (newer examples)
- LVGL v9 (GUI examples)
- Arduino ESP32 core (Arduino framework)
- PlatformIO build system

### Interesting Techniques
- Provides both `usb_mass_storage` and `usb_mass_storage_led` examples, showing USB MSC
  implementation with concurrent LED feedback — directly relevant to Drive Mode in this project.
- `fs_webserver` example demonstrates serving files from the SD card via HTTP — a close
  analogue to the file manager feature.
- Board definition pins `ARDUINO_USB_MODE=1` to enable hardware USB directly from Arduino
  framework, avoiding manual TinyUSB init in most cases.

### Relevance
**Very High.** This is the authoritative hardware reference. The `usb_mass_storage` and
`fs_webserver` examples are direct analogues of the two core modes of the Smart USB Drive.
The board definition JSON confirms the correct CPU speed, flash mode, and USB flags for
this exact hardware.

---

## Project 2 — USB Army Knife

**URL:** https://github.com/i-am-shodan/USBArmyKnife

### What It Is
A professional-grade, multi-mode penetration testing platform built on the T-Dongle-S3 (recommended)
and several other ESP32-S3/S2 boards. The most fully-featured open-source firmware for this
hardware class.

### Hardware Targeted
- LilyGO T-Dongle-S3 (recommended primary target)
- Evil Crow Cable Wind (ESP32-S3 in USB cable)
- T-Watch S3
- Waveshare ESP32-S3 boards
- M5Stack AtomS3U
- RP2040-GEEK (limited support)
- ESP32-S2 variants (budget option, reduced features)

### Key Features

**USB Modes:**
- USB HID keyboard (DuckyScript injection, 50+ keyboard layouts)
- USB Mass Storage / CDROM emulation (covert storage, payload delivery)
- USB Network (NCM — appears as Ethernet adapter, enables PCAP capture)
- USB Serial (persistent backdoor channel)

**WiFi/BT:**
- ESP32 Marauder integration (deauth, evil AP, beacon spam, packet capture)
- Thermal throttling to prevent overheating in AP mode
- Access point SSID masquerades as "iPhone14"

**Web Interface:**
- Bootstrap-based responsive UI at `http://4.3.2.1:8080`
- Real-time attack status, uptime monitoring
- File manager for scripts and results
- Available on ESP32-S3 only (S2 lacks RAM)

**Agent/Post-Exploitation:**
- Serial agent deploys persistent backdoor over USB
- VNC-over-WiFi screen capture
- Microphone audio streaming (M5Stack AtomS3U only)

### Libraries / Frameworks
- PlatformIO (build system, multi-environment `platformio.ini`)
- Bootstrap (web UI)
- ESP32 Marauder (forked, WiFi/BT attack engine)
- Native ESP-IDF (USB device/host stack)
- FreeRTOS (implicit via ESP-IDF — concurrent USB + WiFi tasks)

### Interesting Techniques
- **Multi-device USB spoofing**: Device masquerades as different storage volumes on each
  successive plug, defeating blocklist-based defenses.
- **Thermal throttling**: Active monitoring of chip temperature; AP mode is throttled before
  overheating. Directly applicable to the Smart USB Drive (no hardware thermal protection on T-Dongle-S3).
- **Hardware abstraction layer**: Single codebase supports 7+ different hardware variants via
  board-specific config profiles in `platformio.ini` environments — clean approach for future
  hardware ports.
- **DuckyScript augmentation**: Standard HID scripting language extended with WiFi/BT commands,
  showing how a domain-specific scripting system can be extended without breaking compatibility.
- **Port 8080 for admin UI**: Separating the admin web server from port 80 — identical pattern
  to this project's port 8080 synchronous `dl_server`.

### Repository Layout
```
data/          - Static data files
include/       - Header files
lib/           - Library dependencies
src/           - Main source code
test/          - Test files
tools/         - Utility tools
ui/            - Web UI components
examples/      - Attack script examples
extra_scripts.py - PlatformIO build customization
```

### Relevance
**Very High.** The most architecturally complex open-source firmware for this exact hardware.
Key takeaways for the Smart USB Drive:
1. Thermal throttling pattern applicable since T-Dongle-S3 has no hardware thermal shutdown.
2. Port 8080 admin separation is already adopted in this project.
3. FreeRTOS task partitioning for concurrent USB + WiFi (the core architectural constraint of the Smart USB Drive) is solved here via hardware abstraction.
4. Bootstrap web UI with file manager is a production-proven approach on this hardware.

---

## Project 3 — T-Dongle-Ducky

**URL:** https://github.com/LockManipulator/T-Dongle-Ducky

### What It Is
A simple rubber ducky (HID keyboard injector) with a WiFi-controlled web interface, built
for the T-Dongle-S3. This is the spiritual ancestor of PwnStick-Lua (#8 below).

### Hardware Targeted
- LilyGO T-Dongle-S3
- Uses ESP32-S3-USB-OTG board config in Arduino IDE

### Key Features
- USB HID keyboard emulation via ESP32-S3 native USB-OTG
- WiFi Access Point with web authentication (default: root/toor)
- Custom scripting language for keystroke injection:
  - `PRINT` / `PRINTLN` — type text
  - `GUI`, `CTRL`, `ALT`, `SHIFT` — modifier keys
  - `F1`–`F12` — function keys
  - `DELAY` — timing control
- SD card file storage (files must have leading slash and extension)
- Planned (roadmap): Bluetooth, mouse emulation, USB MSC mount, shell access, OS detection

### Libraries Used
- Arduino IDE framework
- Official T-Dongle-S3 LilyGO board support
- Pure C++ (100% of codebase)

### Interesting Techniques
- **AP + web auth pattern**: Basic credential gate (root/toor) before any web access —
  simple but effective for a device that broadcasts its own SSID.
- **File path convention**: Enforcing leading slash + extension in file naming prevents
  path traversal ambiguity on FAT32 — a simple sanity constraint worth noting for web-based
  file upload endpoints.
- **Planned MSC coexistence**: The roadmap explicitly lists "USB mass storage mounting" as
  a future feature, acknowledging the same HID-vs-MSC coexistence challenge this project
  already solved.

### Relevance
**Medium.** Not directly applicable to the Smart USB Drive architecture, but the web-auth
pattern and file naming constraint are useful reference points. The T-Dongle-Ducky's planned
roadmap mirrors several Phase 4/5 features of this project (MSC, shell access, OS detection).

---

## Project 4 — ZeroTrace

**URL:** https://www.zerotrace.pw/

### What It Is
A commercial (one-time purchase, no subscription) hardware + cloud software platform for
authorized HID automation, USB emulation, and security testing. German-made, targets
professional red-team and IT automation use cases.

### Hardware
- ESP32-S3 dual-core @ 240 MHz
- 16 MB flash, 512 KB SRAM
- Similar spec to T-Dongle-S3

### Key Features
- WiFi-enabled remote keystroke/mouse injection
- Multiple switchable firmware configurations
- Cloud development suite: scripting editor, debugger, emulator
- Webhook integration (remote trigger payloads)
- Cross-platform: Windows, macOS, Linux, Android
- 50+ keyboard layouts
- Monthly feature updates

### Libraries / Frameworks
Not disclosed (closed-source commercial product).

### Interesting Techniques
- **Webhook integration**: `POST /trigger` → execute payload. This is a commercial proof
  that the webhook download pattern (`POST /api/download`) in Phase 4 of the Smart USB Drive
  is the right design direction.
- **Cloud scripting editor**: Offloads script development complexity from the device to
  a hosted web IDE — a potential future direction for the Smart USB Drive's manifest/auto-pack
  feature.
- **Multiple firmware switching**: Users can flash specialized firmware images for different
  scenarios without losing configuration — analogous to the Smart USB Drive's dual-mode
  (USB Drive / Network) switch saved in NVS.

### Relevance
**Low-Medium.** Closed source, so no code to reference. Useful as a commercial feature
benchmark: webhook triggers, cloud scripting, and multi-firmware switching are all validated
market features. The webhook download (`POST /api/download`) in Phase 4 directly parallels
ZeroTrace's webhook trigger design.

---

## Project 5 — PS5-Dongle

**URL:** https://github.com/stooged/PS5-Dongle

### What It Is
Firmware that turns the T-Dongle-S3 into a compact PS5 exploit delivery device (kernel
exploits for firmware 3.xx–4.xx). The device simultaneously serves as a USB mass storage
drive (for easy payload updates) and runs an HTTPS web server for exploit delivery.

### Hardware Targeted
- LilyGO T-Dongle-S3 (both LCD and non-LCD variants)
- Requires FAT32 microSD card

### Key Features
- WiFi Access Point mode with configurable SSID/IP
- HTTPS server (custom fork) + DNS server
- USB Mass Storage via SD card
- Web admin panel (5 HTML routes — see below)
- Firmware OTA update via web UI (accepts `fwupdate.bin`)
- SD-based payload management (copy files via USB drive — no re-flashing needed)

### Web Routes
| Route | Function |
|-------|---------|
| `admin.html` | Primary control dashboard |
| `info.html` | Device/board diagnostics |
| `update.html` | OTA firmware update |
| `config.html` | WiFi AP + IP configuration |
| `reboot.html` | Device restart |

### Libraries Used
- ESP32 HTTPS Server (custom fork by stooged)
- ESPAsyncWebServer (async HTTP)
- AsyncTCP
- ESP32 Arduino core ≤ 2.0.14

### Interesting Techniques
- **SD-based payload update model**: No firmware re-flash needed — users copy payload files
  to the SD card via USB drive mode. Identical to the Smart USB Drive's core value proposition.
- **HTML pages on SD card**: Web UI HTML files live on the SD card, not in flash PROGMEM.
  This enables web UI updates without reflashing — a pattern worth considering for the Smart
  USB Drive's file manager UI.
- **ESP32 core ≤ 2.0.14 requirement**: A known compatibility constraint that this project
  resolved by migrating to pioarduino 55.03.37 (Core 3.x). The stooged approach is locked
  to older Core 2.x due to HTTPS server library constraints.
- **OTA firmware update**: `update.html` + `fwupdate.bin` pattern is a clean OTA mechanism.
  The Smart USB Drive lacks this feature — it would be a Phase 5 candidate.
- **DNS server alongside HTTPS**: Provides hostname resolution in AP mode — same mDNS goal
  as `usbdrive.local` in the Smart USB Drive but via a captive-portal DNS approach.

### Relevance
**High.** This project is architecturally the closest analogue to the Smart USB Drive:
same hardware, same dual MSC+web pattern, same SD-based file update model. Key inspiration:
- OTA firmware update via web UI (not yet in Smart USB Drive)
- HTML on SD card (reduces flash usage, allows web UI updates without reflash)
- Config/reboot pages as separate HTML routes

---

## Project 6 — ds-scripts-USB-Army-Knife

**URL:** https://github.com/Nikorasu-07/ds-scripts-USB-Army-Knife

### What It Is
A collection of DuckyScript (`.ds`) payload files designed to run on a T-Dongle-S3 running
USB Army Knife firmware. Not a firmware project — purely an attack script library.

### Hardware Targeted
- LilyGO T-Dongle-S3 running USB Army Knife firmware

### Key Features
Three script categories:
1. Admin user creation (Windows)
2. Elevated PowerShell access
3. Windows Defender disablement

- Italian keyboard layout optimization via `RAW_HID` mode
- Emphasis on `DELAY` calibration per target system performance
- Windows-only; macOS confirmed non-functional; Linux untested

### Libraries Used
- DuckyScript syntax (USB Army Knife's extended dialect)
- None (pure script files, no firmware)

### Interesting Techniques
- **RAW_HID mode for precise character mapping**: When target keyboard layout doesn't match
  DuckyScript's character table, `RAW_HID` sends raw HID scan codes bypassing layout
  interpretation — useful for non-US keyboard targets.
- **DELAY as a first-class concern**: Documents that timing is system-dependent and must
  be calibrated. A reminder that SD write delays and network latency in the Smart USB Drive
  similarly need per-environment tuning.

### Relevance
**Low.** Script collection only, no firmware architecture. Provides context on how USB Army
Knife's scripting system is used in practice, but no direct applicability to the Smart USB Drive.

---

## Project 7 — PS4-Dongle

**URL:** https://github.com/stooged/PS4-Dongle

### What It Is
The PS4 variant of stooged's dongle firmware (same author as PS5-Dongle). Targets PS4 9.00
OOB exploit and PsFree webkit exploitation chain. Architecturally identical to PS5-Dongle.

### Hardware Targeted
- LilyGO T-Dongle-S3 (with or without LCD)
- FAT32 microSD card required

### Key Features
- WiFi AP with HTTP/HTTPS server + DNS
- USB Mass Storage (SD card exposed as USB drive)
- Web admin panel (same 6 routes as PS5-Dongle)
- Firmware OTA update
- Payload listing via auto-generated `index.html` (fallback if no custom index)

### Web Routes
| Route | Function |
|-------|---------|
| `admin.html` | Control dashboard |
| `index.html` | Auto-generated payload listing |
| `info.html` | Board diagnostics |
| `update.html` | OTA via `fwupdate.bin` |
| `config.html` | WiFi + IP config |
| `reboot.html` | Device restart |

### Libraries Used
- ESPAsyncWebServer
- AsyncTCP
- ESP32 Arduino core ≤ 2.0.14

### Interesting Techniques
- **Auto-generated index.html fallback**: If no custom `index.html` exists on the SD card,
  the firmware generates a payload listing page automatically. This is analogous to the Smart
  USB Drive's file manager auto-listing behavior.
- **`fwupdate.bin` OTA pattern**: Simple, discoverable OTA filename convention — same concept
  could be added to the Smart USB Drive as a Phase 5 feature.
- **Identical architecture to PS5-Dongle**: Confirms stooged's approach is a stable,
  reusable pattern for T-Dongle-S3 dual MSC+web devices.

### Relevance
**High** (same as PS5-Dongle). Together these two projects form a validated reference for
the dual MSC+web server architecture on this exact hardware. The auto-generated index fallback
and OTA update pattern are both Phase 5 candidates for the Smart USB Drive.

---

## Project 8 — pwnstick-lua

**URL:** https://github.com/Rilshrink/pwnstick-lua

### What It Is
A rubber ducky clone for T-Dongle-S3 that replaces DuckyScript with an embedded Lua VM for
scripting. Extends LockManipulator's T-Dongle-Ducky with Lua scripting and a more complete
web interface. Inspired by WiFiDuck design principles.

### Hardware Targeted
- LILYGO T-Dongle-S3 (ESP32-S3-USB-OTG board config)
- FAT32 microSD card

### Key Features
- WiFi AP (SSID: "PwnStick", PSK: "pwnedpwned") with web interface at `192.168.1.1`
- HTTP Basic Auth gate (root/toor)
- Lua scripting API: simultaneous keyboard + mouse control
- Autorun: `autorun.lua` executes automatically on device insertion
- SD card file manager embedded in web UI
- Script upload and execution via web interface
- Optional TFT display for status output

### Libraries Used
- T-Dongle-S3 board support (LilyGO)
- ESPAsyncWebServer
- AsyncTCP
- ESP8266-Arduino-Lua (Lua VM embedded into ESP32 build)

### Interesting Techniques
- **Lua VM on ESP32-S3**: The ESP8266-Arduino-Lua library embeds a complete Lua interpreter.
  The author notes "the ESP32-S3 is more than powerful enough to run a full lua VM." This
  pattern could apply to the Smart USB Drive's planned manifest auto-pack feature — a Lua
  or JS-like script on the SD card could define multi-URL download batches.
- **Autorun pattern**: `autorun.lua` on SD root executes automatically. The Smart USB Drive
  already has an analogous `_switch_network.txt` magic-bytes trigger. A general `autorun.json`
  manifest (list of URLs to download) would follow the same pattern.
- **SD-based web file manager**: The web interface includes file browser/upload for the
  SD card — same feature set as the Smart USB Drive's Phase 3 file manager.
- **ESPAsyncWebServer + AsyncTCP**: Same library stack as the Smart USB Drive, validating
  the choice.

### Relevance
**Medium-High.** The Lua autorun pattern is directly inspirational for the Smart USB Drive's
planned `manifest.json` auto-pack feature. The ESP8266-Arduino-Lua approach shows that a
full scripting VM is feasible on this hardware if future extensibility is desired.

---

## Project 9 — T-Dongle-S3-Firmware (Binary Archive)

**URL:** https://github.com/kaliwinki-1/T-Dongle-S3-Firmware

### What It Is
A personal binary archive of pre-compiled firmware images for the T-Dongle-S3, collected
from multiple open-source projects. Not original code — a convenience aggregator.

### Featured Firmware Images
| Firmware | Capabilities |
|---------|-------------|
| USB Army Knife | HID attacks, BadUSB, MSC emulation |
| UltraWiFiDuck | Advanced DuckyScript injection over WiFi |
| WiFiExe | Execute payloads remotely via web interface |
| ESPloitV2/WHID-Master | WiFi HID injection and remote control |

### Technical Approach
- All images flashed via **ESP Web Tool** at offset `0x0`
- Switching firmware requires full flash erasure
- Each image has its own AP credentials (e.g., password: "12345678")

### Interesting Observations
- **No single firmware does everything**: The fact that users need to switch between 4+
  firmware images to cover different use cases confirms there is no single comprehensive
  open-source solution for the T-Dongle-S3 — the Smart USB Drive fills a distinct niche
  (file management + URL download + WiFi server, not attack tooling).
- **Flash-erase firmware switching**: A primitive but functional multi-firmware approach.
  The Smart USB Drive's dual-mode NVS switching is architecturally superior (no reflash
  needed, sub-second switch).

### Relevance
**Low.** No original code. Useful as a market survey showing what the T-Dongle-S3 community
currently uses. Confirms the Smart USB Drive's niche is not crowded by existing open-source
alternatives.

---

## Project 10 — Bruce v1.0 for LilyGO T-Dongle-S3

**URL:** https://github.com/Mecres256/Bruce-v1.0-for-LilyGO-T-Dongle-S3

### What It Is
A GUI overlay that emulates Bruce Firmware (a popular ESP32 multi-tool) on the T-Dongle-S3,
which cannot natively run Bruce due to hardware differences. Built on top of USB Army Knife
firmware with a custom autorun DuckyScript that drives a menu system using PNG images displayed
on the LCD.

### Hardware Targeted
- LilyGO T-Dongle-S3 with LCD
- Requires microSD card

### Key Features
Menu categories:
- WiFi (beacon spam, deauth attacks)
- Bluetooth (device spoofing)
- Broadcasting
- Utilities
- Windows (USB automation)
- Android

- USB/BLE/WiFi status indicators on display
- Evil portal (fake Apple auth page)
- Apple Watch remote execution via web interface at `4.3.2.1:8080`
- Device unlock automation
- Modular command structure via editable config files

### Architecture
```
Base firmware:  USB Army Knife (pre-flashed)
Control script: autorun.ds (DuckyScript menu logic)
UI assets:      PNG images in files/, more/, select/ directories
```

### Interesting Techniques
- **PNG-based menu UI over LCD**: Menu items are PNG images stored on the SD card,
  rendered on the ST7735 LCD. This decouples UI from firmware — updating a menu item
  means replacing a PNG file, not reflashing. The Smart USB Drive uses LovyanGFX with
  text-based LCD rendering; a sprite/image approach could support richer UI in future phases.
- **DuckyScript as a menu controller**: The `autorun.ds` file drives the entire menu logic
  — each menu choice executes a different DuckyScript. Shows DuckyScript being used for
  general-purpose device control, not just HID injection.
- **Editable config files on SD**: User-customizable behavior via SD-resident config files,
  no reflash required. Analogous to the Smart USB Drive's NVS settings, but file-based
  (more accessible to end users).

### Relevance
**Medium.** The SD-resident PNG asset approach and editable config file pattern are
interesting for Phase 5 polish features. The image-on-SD approach for UI assets could
reduce flash usage and allow theme updates without reflashing (complementing the Smart
USB Drive's current NVS-based theme system).

---

## Project 11 — T-Dongle-S3-forecaster

**URL:** https://github.com/samsan/T-Dongle-S3-forecaster

### What It Is
A standalone weather forecasting application for the T-Dongle-S3. Fetches live weather data
from the OpenWeatherMap API and displays forecasts on the device's ST7735 LCD.

### Hardware Targeted
- T-Dongle-S3 (ESP32-S3, ST7735 display)

### Key Features
- WiFi STA mode connection to home network
- OpenWeatherMap API (HTTP GET, JSON response)
- Weather condition icons stored as embedded bitmaps in a header file
- Small TFT display rendering of current conditions and forecast

### Libraries Used
- TFT_eSPI (version ≤ 2.0.14, same constraint as other pre-Core-3.x projects)
- Arduino HTTP client (built-in)
- ArduinoJson (implied, for API response parsing)

### Configuration Pattern
Three separate header files:
- `secrets.h` — WiFi credentials and API key (renamed from `secrets_example.h`)
- `weatherParams.h` — location settings (renamed from `weatherParamsExample.h`)
- `pin_config.h` — hardware pin mappings

### Interesting Techniques
- **Secrets file pattern**: The `secrets_example.h` → `secrets.h` rename pattern is a clean
  way to keep credentials out of version control while providing a template. The Smart USB
  Drive stores credentials in NVS (superior for end users who configure via web UI), but the
  `secrets_example.h` pattern is useful for developer-facing configuration (e.g., Telegram
  bot token).
- **Embedded bitmap icons**: Weather icons stored as C header arrays (`weather_condition_imgs.h`).
  This pattern could be used in the Smart USB Drive to embed file-type icons in the LCD UI
  (Phase 5 "file type icons on LCD" feature).
- **OpenWeatherMap API integration**: Demonstrates the full cycle of WiFi → HTTP GET →
  JSON parse → LCD render on this hardware, at ~14 KB flash cost. Directly shows that
  external API calls (relevant to future Telegram Bot or webhook features) work reliably
  on this platform.

### Relevance
**Low.** Weather display is not in scope. However, the API fetch → JSON parse → display
pipeline and the embedded bitmap icon pattern have direct applicability to Phase 4/5 features
(Telegram Bot HTTP calls, file type icons on LCD).

---

## Cross-Cutting Observations

### 1. The Dual MSC + Web Server Pattern Is Validated at Scale
Projects #5 (PS5-Dongle), #7 (PS4-Dongle), and #2 (USB Army Knife) all implement the
same core architecture: USB MSC for file updates + WiFi HTTP server for control. All use
ESPAsyncWebServer + AsyncTCP — the same stack chosen for the Smart USB Drive. The pattern
is production-proven on T-Dongle-S3 hardware.

### 2. ESP32 Core 2.x vs 3.x Is the Major Ecosystem Split
All projects except the Smart USB Drive are pinned to ESP32 core ≤ 2.0.14. This is because:
- Core 3.x (via pioarduino) changed SD_MMC DMA task-affinity behavior
- Core 3.x changed USB stack initialization requirements
- Core 3.x requires `usb_stubs.c` weak-symbol fixes

The Smart USB Drive is the only T-Dongle-S3 project in this survey using Core 3.x (ESP-IDF 5.5.2),
making it uniquely positioned for future IDF 5.x features (better TCP, updated BT, etc.).

### 3. OTA Firmware Update Is a Universal Feature
Projects #5 and #7 both implement `update.html` + `fwupdate.bin` OTA update. USB Army Knife
also supports OTA. This is a mature, expected feature for any ESP32-based dongle product.
The Smart USB Drive does not yet have OTA — this is a strong Phase 5 candidate.

### 4. Port 8080 for Secondary Services Is a Common Pattern
USB Army Knife serves its web UI at `:8080`. Bruce overlay targets `4.3.2.1:8080`.
The Smart USB Drive independently arrived at the same pattern for `dl_server` (synchronous
SD download server on port 8080). This convergence validates the architectural decision.

### 5. SD-Resident Web UI HTML Is Worth Considering
PS5-Dongle and PS4-Dongle serve HTML pages directly from the SD card rather than embedding
them in PROGMEM. Benefits:
- Web UI updates without reflashing
- Larger HTML/JS files possible (SD = 32 GB vs 16 MB flash)
- Users can customize the UI by editing files in Drive Mode
Tradeoff: requires SD card at all times. The Smart USB Drive embeds HTML in PROGMEM
(via string constants in `web_server.cpp`), which is simpler but couples UI changes to
firmware releases.

### 6. Autorun / Manifest Pattern Is an Established Convention
Multiple projects (`pwnstick-lua`'s `autorun.lua`, `Bruce overlay`'s `autorun.ds`,
`Switch_to_Network_Mode.vbs`) rely on SD-resident files to trigger device behavior.
The Smart USB Drive already uses this for mode switching (`_switch_network.txt`).
Extending it to a `manifest.json` download list (Phase 5 auto-pack) is consistent with
the established convention across this entire ecosystem.

### 7. Thermal Management Is Underserved by Most Projects
Only USB Army Knife explicitly implements thermal throttling. Given that T-Dongle-S3 has
no hardware thermal shutdown, the Smart USB Drive's Phase 5 internal temperature monitoring
feature addresses a real gap in the ecosystem.

### 8. Lua / Scripting VM Is Feasible on This Hardware
PwnStick-Lua runs a full ESP8266-Arduino-Lua VM on ESP32-S3. This demonstrates that
a scripting layer (Lua, MicroPython, or a custom JSON-based DSL) for automating download
batches or configuring device behavior is technically feasible within the RAM budget.

---

## Feature Gap Analysis: Smart USB Drive vs Ecosystem

| Feature | Smart USB Drive | Ecosystem Best-in-Class |
|---------|----------------|------------------------|
| USB MSC | Yes (Drive Mode) | PS5/PS4-Dongle, USB Army Knife |
| HTTP file manager | Yes (full — list/upload/download/delete/rename/mkdir/ZIP/search) | USB Army Knife (partial) |
| URL downloader | Yes (LCD progress + speed/ETA) | None in survey |
| Download queue | Phase 4 TODO | None in survey |
| Web UI | Yes (config + file manager) | USB Army Knife (Bootstrap :8080) |
| OTA firmware update | **No** | PS4/PS5-Dongle, USB Army Knife |
| Telegram Bot | Phase 4 TODO | None in survey |
| Webhook POST | Phase 4 TODO | ZeroTrace (commercial) |
| Auto-pack manifest | Phase 5 TODO | PwnStick-Lua (autorun.lua analog) |
| LCD progress bar | Yes | Bruce (PNG-based menu) |
| Dual mode (USB/Network) | Yes (NVS switch) | All others require reflash |
| Core 3.x / IDF 5.5.2 | **Yes (unique)** | None in survey |
| PSRAM support | Yes (8 MB OPI) | None confirmed in survey |
| TCP window scaling | Yes (512 KB) | None in survey |
| Theme system | Yes (3 palettes, NVS) | None in survey |
| mDNS | Yes (usbdrive.local) | None in survey |
| Thermal monitoring | Phase 5 TODO | USB Army Knife (throttling) |

---

## Recommended Actions Based on This Survey

### High Priority (Phase 4/5)
1. **OTA firmware update**: Add `POST /api/ota` accepting `multipart/form-data` with a
   `.bin` file → write to OTA partition → restart. PS4/PS5-Dongle's `update.html` +
   `fwupdate.bin` pattern is the simplest approach. Leverages ESP32 Arduino `Update` library.

2. **Thermal monitoring**: Implement `temperatureRead()` (built-in ESP32-S3 ROM function)
   in `main.cpp` loop. Warn on LCD if > 75°C. USB Army Knife's throttling approach confirms
   this is a necessary safeguard.

### Medium Priority (Phase 5)
3. **SD-resident HTML option**: Consider adding a compile flag or runtime check: if
   `/web_ui.html` exists on SD card, serve it instead of the PROGMEM-embedded version.
   Would allow web UI updates without reflashing, following PS4/PS5-Dongle's approach.

4. **Embedded file-type icons**: Use the `weather_condition_imgs.h` bitmap pattern (from
   forecaster project) to embed small file-type icons in flash, rendered on LCD in Phase 5.

5. **Autorun manifest**: `manifest.json` on SD root → auto-download list, following
   PwnStick-Lua's `autorun.lua` precedent. Parse with ArduinoJson v7 (already in use).

### Low Priority / Research
6. **Lua scripting VM**: ESP8266-Arduino-Lua is compatible with ESP32-S3 per PwnStick-Lua.
   Could enable user-defined download automation scripts without firmware updates.

7. **Multi-firmware switching**: kaliwinki-1's archive shows demand for multiple firmware
   personalities. A "mode 3" could be a dedicated HID emulator for USB→Network migration
   automation, without requiring a full reflash.
