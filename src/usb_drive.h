#pragma once
#include <Arduino.h>

// Initialise USB Mass Storage — SD card becomes a removable drive on the host PC.
// WiFi must be disabled before calling this.
// SD card must already be mounted (storage_begin() called first).
// Returns true on success.
bool usb_drive_begin();

// Stop USB MSC cleanly.  Call before switching back to Network Mode.
void usb_drive_end();

// True if USB MSC stack is currently active.
bool usb_drive_active();

// True if the host sent a SCSI eject (Safely Remove Hardware).
// When true the main loop should check for trigger file and restart.
bool usb_drive_was_ejected();

// True if the magic-bytes trigger was detected in a write — switch to Network Mode immediately.
bool usb_drive_switch_requested();
