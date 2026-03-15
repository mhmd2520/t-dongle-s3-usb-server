#pragma once
#include <Arduino.h>

// Start the HTTP config server on port 80.
// Works in both STA mode (real IP) and AP mode (192.168.4.1).
// Call after wifi_begin() in Network Mode setup.
void web_server_begin();

// Manage async WiFi scan and deferred restart.
// Must be called from loop() while in Network Mode.
void web_server_loop();
