#include "usb_drive.h"
#include "config.h"
#include <USB.h>
#include <USBMSC.h>
#include <SD_MMC.h>

// Core 3.x: SD_MMC._card is protected; use readRAW/writeRAW/numSectors/sectorSize.

static USBMSC             s_msc;
static bool               s_active  = false;
static volatile bool      s_ejected = false;
static volatile bool      s_switch  = false;   // magic-bytes mode-switch request

static bool on_start_stop(uint8_t /*power_condition*/, bool start, bool load_eject) {
    if (!start && load_eject) s_ejected = true;
    return true;
}

// ── MSC callbacks (called from TinyUSB task context) ─────────────────────────

static int32_t on_read(uint32_t lba, uint32_t offset, void* buf, uint32_t bufsize) {
    uint32_t num_sectors = bufsize / 512;
    uint8_t* dst = (uint8_t*)buf + offset;
    for (uint32_t i = 0; i < num_sectors; i++) {
        if (!SD_MMC.readRAW(dst + i * 512, lba + i)) return -1;
    }
    return (int32_t)bufsize;
}

static int32_t on_write(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t bufsize) {
    // Detect magic string written by Switch_to_Network_Mode.bat — no eject needed.
    if (!s_switch && bufsize > offset + 17) {
        static const char MAGIC[] = "SWITCH_TO_NETWORK";
        const uint8_t* data = buf + offset;
        const uint32_t dlen = bufsize - offset;
        for (uint32_t i = 0; i + 17 <= dlen; i++) {
            if (memcmp(data + i, MAGIC, 17) == 0) { s_switch = true; break; }
        }
    }
    uint32_t num_sectors = bufsize / 512;
    uint8_t* src = buf + offset;
    for (uint32_t i = 0; i < num_sectors; i++) {
        if (!SD_MMC.writeRAW(src + i * 512, lba + i)) return -1;
    }
    return (int32_t)bufsize;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool usb_drive_begin() {
    if (SD_MMC.numSectors() == 0) {
        Serial.println("[USB] SD card not initialised — cannot start MSC");
        return false;
    }

    uint32_t blocks = (uint32_t)SD_MMC.numSectors();

    s_msc.vendorID("LilyGo");
    s_msc.productID("T-Dongle");
    s_msc.productRevision("1.00");
    s_msc.onRead(on_read);
    s_msc.onWrite(on_write);
    s_msc.onStartStop(on_start_stop);
    s_msc.mediaPresent(true);
    s_msc.begin(blocks, 512);

    USB.begin();

    s_active = true;
    Serial.printf("[USB] MSC active — %u blocks (%.1f GB)\n",
                  blocks, (float)blocks * 512.0f / 1073741824.0f);
    return true;
}

void usb_drive_end() {
    s_msc.end();
    s_active = false;
    Serial.println("[USB] MSC stopped");
}

bool usb_drive_active() {
    return s_active;
}

bool usb_drive_was_ejected() {
    return s_ejected;
}

bool usb_drive_switch_requested() {
    return s_switch;
}
