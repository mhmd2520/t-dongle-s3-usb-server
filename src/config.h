#pragma once
#include <Arduino.h>

// ============================================================
// USER SETTINGS
// ============================================================

// WiFi credentials are NO LONGER hardcoded here.
// On first boot the device starts a "USBDrive-Config" access point.
// Connect with any phone/PC and open http://192.168.4.1 to configure WiFi.
// Credentials are saved to flash (NVS) and reused on every subsequent boot.
// To reset: hold the BOOT button for 3 seconds while the device is starting.

#define WIFI_TIMEOUT_MS  10000      // connect timeout for saved credentials (ms)

// Access-point (config portal) identity
#define AP_SSID  "T-Dongle-S3"   // broadcasted SSID
#define AP_PASS  "configure"      // WPA2 password
#define AP_IP    "192.168.4.1"   // fixed AP gateway IP

// ============================================================
// DEVICE IDENTITY
// ============================================================

#define DEVICE_NAME   "USB Drive"
#define FW_VERSION    "1.5.0"
#define MDNS_HOST     "usbdrive"    // → http://usbdrive.local

// ============================================================
// HARDWARE PINS — T-Dongle-S3
// Verify against your board revision if display/SD don't work.
// Official schematic: github.com/Xinyuan-LilyGO/T-Dongle-S3
// ============================================================

// LCD (ST7735) — also set via build_flags for TFT_eSPI
#define PIN_LCD_CS    4
#define PIN_LCD_DC    2
#define PIN_LCD_RST   1
#define PIN_LCD_MOSI  3
#define PIN_LCD_SCLK  5
#define PIN_LCD_BL    38

// SD card — SD_MMC 4-bit mode (official LilyGo T-Dongle-S3 pin mapping)
// These are NOT the same as SPI pins; SD_MMC uses dedicated GPIO matrix routing.
#define PIN_SD_CLK    12
#define PIN_SD_CMD    16
#define PIN_SD_D0     14
#define PIN_SD_D1     17
#define PIN_SD_D2     21
#define PIN_SD_D3     18

// RGB LED — APA102 (SPI: DATA=GPIO40, CLK=GPIO39)
#define PIN_LED_DI    40   // data
#define PIN_LED_CLK   39   // clock

// Boot / mode-switch button
#define PIN_BUTTON    0

// ============================================================
// DISPLAY — dimensions after rotation=0 (portrait)
// ============================================================

#define LCD_W   80
#define LCD_H  160

// ============================================================
// OPERATING MODE
// ============================================================

// Saved to NVS under NVS_NS / NVS_KEY_MODE on every mode switch.
// On boot the device reads this key and initialises the matching stack.
enum AppMode { MODE_NETWORK = 0, MODE_USB_DRIVE = 1 };

// ============================================================
// NVS NAMESPACE (Preferences key-value store)
// ============================================================

#define NVS_NS            "usbdrive"
#define NVS_KEY_MODE      "mode"      // uint8_t — stores AppMode value
#define NVS_KEY_THEME     "theme"     // uint8_t — LCD colour theme index
#define NVS_KEY_AUTH_USER "auth_u"    // string  — web UI username (empty = no auth)
#define NVS_KEY_AUTH_PASS "auth_p"    // string  — web UI password
