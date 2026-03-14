#pragma once
#include <Arduino.h>

// Initialise display, turn on backlight, show splash screen.
void lcd_begin();

// Update the "Starting..." line on the splash screen during boot sequence.
void lcd_splash_msg(const char* msg);

// Main status screen for Network Mode — handles both STA-connected and AP states.
//   wifi_ok      – true if connected to a router
//   ap_mode      – true if running as config access point (overrides wifi_ok display)
//   ip           – STA IP when wifi_ok, AP IP ("192.168.4.1") when ap_mode
//   sd_ok        – true if SD card is mounted
//   sd_free_gb   – free space in GB
//   sd_total_gb  – total size in GB
void lcd_show_status(bool wifi_ok, bool ap_mode, const String& ip,
                     bool sd_ok, float sd_free_gb, float sd_total_gb);

// Full-screen progress bar used during file operations (Phase 3+).
//   label   – short description, e.g. "Downloading..."
//   percent – 0–100
void lcd_show_progress(const String& label, uint8_t percent);

// USB Drive Mode status screen — call after usb_drive_begin().
//   sd_ok        — true if SD card is mounted and passed to MSC
//   sd_free_gb   — free space in GB (shown on screen)
//   sd_total_gb  — total size in GB (shown on screen)
void lcd_show_usb_mode(bool sd_ok, float sd_free_gb, float sd_total_gb);

// Force a full static-layout redraw on the next lcd_show_status() call.
// Must be called whenever the active mode changes so the header updates correctly.
void lcd_invalidate_layout();

// Turn backlight on (255) or off (0).  PWM dimming added in Phase 5.
void lcd_set_backlight(uint8_t brightness);

// Apply the current theme palette to all LCD colour variables and clear the screen.
// Call after theme_load() in setup(), and after theme_save() for live preview.
void lcd_apply_theme();
