#pragma once
#include <stdint.h>

// ── Theme palette ─────────────────────────────────────────────────────────────
// All colours are RGB565 as used by TFT_eSPI.

struct Theme {
    uint16_t    header;   // header bar fill
    uint16_t    accent;   // header text / highlights
    uint16_t    bg;       // screen background
    uint16_t    text;     // primary body text
    uint16_t    ok;       // success / connected
    uint16_t    error_c;  // error / disconnected
    uint16_t    warn;     // warning / caution
    uint16_t    dim;      // secondary labels / separators
    uint16_t    bar;      // progress bar fill
    const char* name;     // display name for web UI
};

// Load theme index from NVS.  Call once before lcd_begin().
void           theme_load();

// Save theme index to NVS and update the in-memory active theme.
void           theme_save(uint8_t id);

// Index of the active theme (0-based).
uint8_t        theme_current_id();

// Number of available themes.
uint8_t        theme_count();

// Active theme palette.
const Theme&   theme_get();
