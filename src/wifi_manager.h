#pragma once
#include <Arduino.h>
#include "config.h"

// Connect to WiFi using credentials stored in NVS (flash).
//
// Behaviour:
//  • No saved credentials  → starts "USBDrive-Config" AP (non-blocking) and
//    returns false.  Call wifi_portal_loop() from loop() to process DNS.
//  • Saved credentials     → tries to connect within timeout_ms.
//    Success → starts mDNS (usbdrive.local) and returns true.
//    Failure → falls back to AP mode (same as above).
//
// HTTP config portal is handled by web_server — call web_server_begin() after
// wifi_begin() regardless of whether it returned true or false.
bool   wifi_begin(uint32_t timeout_ms = WIFI_TIMEOUT_MS);

// Must be called from loop() in Network Mode.
// In AP mode: processes DNS (captive-portal redirect).
// In STA mode: no-op.
void   wifi_portal_loop();

// True if the device is currently in AP (config) mode.
bool   wifi_is_ap_mode();

// Clear saved credentials and restart the device (can be called at any time).
void   wifi_reset_credentials();

// True if WiFi is currently connected to a station.
bool   wifi_connected();

// Current IP address string.
// Returns STA IP when connected, AP IP (192.168.4.1) when in AP mode,
// "0.0.0.0" otherwise.
String wifi_ip();

// Connected SSID, or "" when in AP mode / disconnected.
String wifi_ssid();

// Cleanly disconnect WiFi and stop mDNS.  Used before switching to USB Drive Mode.
void   wifi_disconnect();
