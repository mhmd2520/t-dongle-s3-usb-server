#include "themes.h"
#include "config.h"
#include <Preferences.h>

// ── RGB565 colour constants ───────────────────────────────────────────────────
// Avoids pulling TFT_eSPI into themes.cpp.

#define TC_BLACK   0x0000u
#define TC_WHITE   0xFFFFu
#define TC_CYAN    0x07FFu
#define TC_GREEN   0x07E0u
#define TC_RED     0xF800u
#define TC_YELLOW  0xFFE0u
#define TC_ORANGE  0xFD20u

// ── Palette table ─────────────────────────────────────────────────────────────

static const Theme THEMES[] = {
    //  header    accent     bg        text      ok         error      warn       dim       bar       name
    // 0 — Dark (default): dark-navy header, cyan accent
    { 0x000Fu, TC_CYAN,    TC_BLACK,  TC_WHITE,  TC_GREEN,  TC_RED,    TC_YELLOW, 0x4208u, TC_CYAN,   "Dark"  },
    // 1 — Ocean: deep-blue tones, teal accent
    { 0x0233u, 0x5EFFu,   0x0011u,   TC_WHITE,  TC_GREEN,  TC_RED,    TC_YELLOW, 0x4A69u, 0x5EFFu,   "Ocean" },
    // 2 — Retro: black background, amber/orange throughout
    { 0x4200u, TC_ORANGE,  TC_BLACK,  TC_ORANGE, TC_ORANGE, TC_RED,    TC_ORANGE, 0x6280u, TC_ORANGE, "Retro" },
};

static const uint8_t NUM_THEMES = (uint8_t)(sizeof(THEMES) / sizeof(THEMES[0]));
static uint8_t g_theme_id = 0;

// ── Public API ────────────────────────────────────────────────────────────────

void theme_load() {
    Preferences p;
    p.begin(NVS_NS, true);
    g_theme_id = p.getUChar(NVS_KEY_THEME, 0);
    p.end();
    if (g_theme_id >= NUM_THEMES) g_theme_id = 0;
}

void theme_save(uint8_t id) {
    if (id >= NUM_THEMES) id = 0;
    g_theme_id = id;
    Preferences p;
    p.begin(NVS_NS, false);
    p.putUChar(NVS_KEY_THEME, id);
    p.end();
}

uint8_t theme_current_id() { return g_theme_id; }
uint8_t theme_count()      { return NUM_THEMES; }
const Theme& theme_get()   { return THEMES[g_theme_id]; }
