#include "storage.h"
#include "config.h"
#include <SD_MMC.h>

static bool     g_ready       = false;
static uint64_t g_total_bytes = 0;
static uint64_t g_used_bytes  = 0;

bool storage_begin() {
    // T-Dongle-S3 uses SD_MMC 4-bit mode, NOT SPI.
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD,
                   PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3);

    if (!SD_MMC.begin()) {
        Serial.println("[SD] Mount failed — card not inserted?");
        g_ready = false;
        return false;
    }

    g_total_bytes = SD_MMC.totalBytes();
    g_used_bytes  = SD_MMC.usedBytes();
    g_ready = true;

    storage_print_info();
    return true;
}

bool storage_is_ready() {
    return g_ready;
}

float storage_total_gb() {
    return (float)g_total_bytes / 1073741824.0f;
}

float storage_free_gb() {
    return (float)(g_total_bytes - g_used_bytes) / 1073741824.0f;
}

void storage_print_info() {
    if (!g_ready) {
        Serial.println("[SD] Not mounted");
        return;
    }

    uint8_t type = SD_MMC.cardType();
    const char* type_str = "UNKNOWN";
    if      (type == CARD_MMC)  type_str = "MMC";
    else if (type == CARD_SD)   type_str = "SD";
    else if (type == CARD_SDHC) type_str = "SDHC";

    Serial.printf("[SD] Type: %s | Total: %.1f GB | Free: %.1f GB\n",
                  type_str, storage_total_gb(), storage_free_gb());
}
