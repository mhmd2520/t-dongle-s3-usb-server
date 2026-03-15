#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "themes.h"
#include "lcd.h"
#include "led.h"
#include "storage.h"
#include "wifi_manager.h"
#include "usb_drive.h"
#include "web_server.h"
#include <SD_MMC.h>

// ── Global state ──────────────────────────────────────────────────────────────

static AppMode   g_mode         = MODE_NETWORK;
static bool      g_wifi_ok      = false;
static bool      g_sd_ok        = false;
static uint32_t  g_last_refresh = 0;

// ── NVS mode persistence ──────────────────────────────────────────────────────

static AppMode load_mode() {
    Preferences p;
    p.begin(NVS_NS, true);
    AppMode m = (AppMode)p.getUChar(NVS_KEY_MODE, MODE_NETWORK);
    p.end();
    return m;
}

static void save_mode(AppMode m) {
    Preferences p;
    p.begin(NVS_NS, false);
    p.putUChar(NVS_KEY_MODE, (uint8_t)m);
    p.end();
}

// ── Mode switch (save → restart) ─────────────────────────────────────────────

static void switch_mode() {
    AppMode next = (g_mode == MODE_NETWORK) ? MODE_USB_DRIVE : MODE_NETWORK;
    Serial.printf("[MODE] Switching to %s — restarting...\n",
                  next == MODE_USB_DRIVE ? "USB DRIVE" : "NETWORK");
    save_mode(next);
    lcd_invalidate_layout();
    lcd_splash_msg("Switching...");
    delay(600);
    ESP.restart();
}

// ── Network Mode helpers ──────────────────────────────────────────────────────

static void refresh_status() {
    g_wifi_ok      = wifi_connected();
    g_last_refresh = millis();
    lcd_show_status(g_wifi_ok, wifi_is_ap_mode(), wifi_ip(),
                    g_sd_ok, storage_free_gb(), storage_total_gb());
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("\n=== %s v%s ===\n", DEVICE_NAME, FW_VERSION);

    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // 1. Load theme, then init LCD + LED (theme affects splash colours)
    theme_load();
    led_begin();
    led_set(0, 0, 80);   // blue = booting
    lcd_begin();

    // 2. Hold BOOT for 3 s at startup → reset WiFi credentials
    if (digitalRead(PIN_BUTTON) == LOW) {
        lcd_splash_msg("Hold BOOT 3s");
        uint32_t t0 = millis();
        while (digitalRead(PIN_BUTTON) == LOW) {
            if (millis() - t0 > 3000) {
                lcd_splash_msg("WiFi reset!");
                delay(800);
                wifi_reset_credentials();   // clears NVS and restarts
            }
        }
        lcd_splash_msg("Starting...");
    }

    // 3. SD card — required by both modes
    lcd_splash_msg("Mounting SD...");
    g_sd_ok = storage_begin();
    Serial.printf("[SD] %s\n", g_sd_ok ? "OK" : "FAILED");

    // 4. Read saved mode from NVS
    g_mode = load_mode();
    Serial.printf("[MODE] %s\n", g_mode == MODE_USB_DRIVE ? "USB DRIVE" : "NETWORK");

    if (g_mode == MODE_USB_DRIVE) {
        // ── USB Drive Mode boot ───────────────────────────────────────────────
        // WiFi never starts — USB and WiFi cannot coexist on ESP32-S3.
        lcd_splash_msg("USB Drive...");

        // Trigger file: if PC user created /_switch_network.txt on the SD drive
        // while it was mounted, switch to Network Mode on this boot.
        if (g_sd_ok && SD_MMC.exists("/_switch_network.txt")) {
            SD_MMC.remove("/_switch_network.txt");
            save_mode(MODE_NETWORK);
            Serial.println("[MODE] Trigger file found — switching to Network Mode");
            lcd_splash_msg("To Network...");
            delay(500);
            ESP.restart();
        }

        // Always recreate the Network Mode switch script so content stays current.
        // Double-clicking this bat writes SWITCH_TO_NETWORK to the SD; Windows FAT32
        // lazy-flushes the write to the device in ~20 s, triggering on_write magic-bytes
        // detection which immediately restarts into Network Mode.
        if (g_sd_ok) {
            File bf = SD_MMC.open("/Switch_to_Network_Mode.bat", FILE_WRITE);
            if (bf) {
                bf.print("@echo off\r\n");
                bf.print("echo Switching USB Smart Drive to Network Mode...\r\n");
                bf.print("echo Device will restart in Network Mode in ~20 seconds.\r\n");
                bf.print("echo SWITCH_TO_NETWORK > \"%~d0\\_switch_network.txt\"\r\n");
                bf.print("pause\r\n");
                bf.close();
            }
        }

        if (g_sd_ok && usb_drive_begin()) {
            led_set(0, 0, 80);   // blue = USB active
        } else {
            led_set(80, 0, 0);   // red = error (no SD card)
        }
        lcd_show_usb_mode(g_sd_ok, storage_free_gb(), storage_total_gb());

    } else {
        // ── Network Mode boot ─────────────────────────────────────────────────
        led_set(80, 40, 0);   // orange = connecting
        lcd_splash_msg("WiFi connect...");
        g_wifi_ok = wifi_begin();

        // Web config server runs in both STA and AP modes.
        web_server_begin();

        if (g_wifi_ok) {
            led_set(0, 80, 0);   // green = connected to router
        } else {
            led_set(80, 40, 0);  // orange = AP mode (no router)
        }

        // Unified status page — shows STA IP when connected, AP name/pass/IP
        // when in AP config mode.  Never blank after wifi_begin() returns.
        lcd_show_status(g_wifi_ok, wifi_is_ap_mode(), wifi_ip(),
                        g_sd_ok, storage_free_gb(), storage_total_gb());
        g_last_refresh = millis();
    }
}

// ── Loop ─────────────────────────────────────────────────────────────────────

void loop() {
    // ── Mode-specific work ────────────────────────────────────────────────────
    if (g_mode == MODE_NETWORK) {
        wifi_portal_loop();    // DNS captive-portal redirect in AP mode (no-op in STA)
        web_server_loop();
        if (millis() - g_last_refresh > 15000) {
            refresh_status();
        }
    }
    // USB Drive Mode: magic-bytes trigger (bat file) → switch to Network Mode immediately.
    if (g_mode == MODE_USB_DRIVE && usb_drive_switch_requested()) {
        usb_drive_end();
        // Remount SD so FatFS cache reflects the raw-sector writes made by the PC.
        // Without remount, SD_MMC.remove() silently fails (file not in FatFS cache).
        SD_MMC.end();
        delay(200);
        storage_begin();
        SD_MMC.remove("/_switch_network.txt");
        save_mode(MODE_NETWORK);
        lcd_splash_msg("To Network...");
        delay(800);
        ESP.restart();
    }
    // USB Drive Mode: SCSI eject (Safely Remove Hardware) → check trigger file.
    if (g_mode == MODE_USB_DRIVE && usb_drive_was_ejected()) {
        usb_drive_end();
        SD_MMC.end();
        delay(200);
        if (storage_begin() && SD_MMC.exists("/_switch_network.txt")) {
            SD_MMC.remove("/_switch_network.txt");
            save_mode(MODE_NETWORK);
            lcd_splash_msg("To Network...");
            delay(500);
        }
        ESP.restart();
    }

    // ── BOOT button ───────────────────────────────────────────────────────────
    if (digitalRead(PIN_BUTTON) == LOW) {
        uint32_t t0 = millis();
        while (digitalRead(PIN_BUTTON) == LOW) {
            // Fire WiFi reset immediately at 3 s — no need to release first.
            if (millis() - t0 >= 3000) {
                lcd_splash_msg("WiFi reset!");
                delay(800);
                wifi_reset_credentials();   // clears NVS and restarts — never returns
            }
            delay(10);
        }
        // Only reach here if button released before 2 s → short press → switch mode.
        uint32_t held = millis() - t0;
        if (held >= 200) switch_mode();  // 200 ms min to avoid contact-bounce false trigger
    }
}
