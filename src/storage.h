#pragma once
#include <Arduino.h>

// Mount the SD card via SD_MMC 4-bit mode (T-Dongle-S3 hardware interface).
// Returns true on success.
bool   storage_begin();

// True if the SD card is mounted and ready.
bool   storage_is_ready();

// Total card size in GB.
float  storage_total_gb();

// Free (unused) space in GB.
float  storage_free_gb();

// Print card info to Serial (type, size, free).
void   storage_print_info();
