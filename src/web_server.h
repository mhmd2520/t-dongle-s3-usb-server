#pragma once
#include <Arduino.h>

// Start the HTTPS config server on port 443 (self-signed cert, CN=usbdrive.local).
// Also starts a plain HTTP server on port 80 that redirects everything to HTTPS.
// Works in both STA mode (real IP) and AP mode (192.168.4.1).
// Call after wifi_begin() in Network Mode setup.
void web_server_begin();

// Process pending requests on both servers.
// Must be called from loop() while in Network Mode.
void web_server_loop();
