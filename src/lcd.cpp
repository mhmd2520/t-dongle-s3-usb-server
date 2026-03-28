#include "lcd.h"
#include "config.h"
#include "themes.h"
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <qrcode.h>

// ── LovyanGFX display config for T-Dongle-S3 (ST7735S 80×160) ────────────────
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7735S _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;
public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 27000000;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = true;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = PIN_LCD_SCLK;
            cfg.pin_mosi    = PIN_LCD_MOSI;
            cfg.pin_miso    = -1;
            cfg.pin_dc      = PIN_LCD_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs      = PIN_LCD_CS;
            cfg.pin_rst     = PIN_LCD_RST;
            cfg.pin_busy    = -1;
            cfg.panel_width  = 80;
            cfg.panel_height = 160;
            cfg.offset_x     = 26;   // ST7735_GREENTAB160x80 colstart
            cfg.offset_y     = 1;    // ST7735_GREENTAB160x80 rowstart
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable    = false;
            cfg.invert      = true;
            cfg.rgb_order   = false;  // BGR
            cfg.dlen_16bit  = false;
            cfg.bus_shared  = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = PIN_LCD_BL;
            cfg.invert      = true;   // active LOW
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

// ── Colour palette — loaded from active theme via lcd_apply_theme() ───────────
static uint16_t C_BG     = 0x0000u;
static uint16_t C_HEADER = 0x000Fu;
static uint16_t C_ACCENT = 0x07FFu;
static uint16_t C_TEXT   = 0xFFFFu;
static uint16_t C_OK     = 0x07E0u;
static uint16_t C_WARN   = 0xFFE0u;
static uint16_t C_ERROR  = 0xF800u;
static uint16_t C_DIM    = 0x4208u;
static uint16_t C_BAR    = 0x07FFu;

// Portrait 80×160 layout constants
static const uint8_t  HDR_H  = 14;    // header bar height (px)
static const uint8_t  FTR_Y  = 148;   // footer separator y
static const uint8_t  FTR_TY = 154;   // footer text centre y

static LGFX tft;
static bool     s_layout_drawn   = false;  // reset by lcd_invalidate_layout() on mode switch
static uint8_t  s_prog_last_pct  = 255;    // 255 = not yet drawn; reset by lcd_invalidate_layout()
static char     s_prog_last_lbl[80] = "";  // cached label for partial-refresh
static int      s_prog_last_spd  = -2;     // -2 = never drawn
static int32_t  s_prog_last_byt  = -1;
static int      s_prog_last_queue = -1;    // cached queue_remaining for header partial-refresh
static char     s_prog_last_hdr[20]  = ""; // cached header string for partial-refresh
static char     s_prog_last_size[18] = ""; // cached size row string — skip SPI if unchanged
static char     s_prog_last_spds[18] = ""; // cached speed row string
static char     s_prog_last_eta[18]  = ""; // cached ETA row string

// ── Helpers ──────────────────────────────────────────────────────────────────

// mode_str — shown in header, e.g. "NETWORK MODE", "USB DRIVE", "Booting..."
static void draw_header(const char* mode_str) {
    tft.fillRect(0, 0, LCD_W, HDR_H, C_HEADER);
    tft.setTextColor(C_ACCENT, C_HEADER);
    tft.setTextDatum(ML_DATUM);
    tft.setTextSize(1);
    tft.drawString(mode_str, 3, HDR_H / 2);
}

// Clear only the content area between header and footer — avoids full-screen blink.
static void clear_content() {
    tft.fillRect(0, HDR_H, LCD_W, FTR_Y - HDR_H, C_BG);
}

static void draw_footer(const char* hint) {
    tft.drawFastHLine(0, FTR_Y, LCD_W, C_DIM);
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextDatum(ML_DATUM);
    tft.setTextSize(1);
    tft.drawString(hint, 3, FTR_TY);
}

// ── Public API ───────────────────────────────────────────────────────────────

void lcd_apply_theme() {
    const Theme& t = theme_get();
    C_BG     = t.bg;
    C_HEADER = t.header;
    C_ACCENT = t.accent;
    C_TEXT   = t.text;
    C_OK     = t.ok;
    C_WARN   = t.warn;
    C_ERROR  = t.error_c;
    C_DIM    = t.dim;
    C_BAR    = t.bar;
    s_layout_drawn = false;
    tft.fillScreen(C_BG);   // immediate visual clear
}

void lcd_begin() {
    tft.begin();
    tft.setRotation(0);               // portrait 80×160, USB plug at top
    lcd_apply_theme();                // sets all C_xxx vars + fills screen

    draw_header("Booting...");

    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);

    tft.setTextColor(C_TEXT, C_BG);
    tft.drawString("T-Dongle-S3", LCD_W / 2, 70);

    tft.setTextColor(C_DIM, C_BG);
    tft.drawString("USB/FTP/Web", LCD_W / 2, 84);
    tft.drawString("Download/Bot", LCD_W / 2, 96);

    draw_footer("Starting...");
}

void lcd_splash_msg(const char* msg) {
    // Clear only the footer text area, then redraw
    tft.fillRect(0, FTR_Y + 1, LCD_W, LCD_H - FTR_Y - 1, C_BG);
    tft.setTextColor(C_WARN, C_BG);
    tft.setTextDatum(ML_DATUM);
    tft.setTextSize(1);
    tft.drawString(msg, 3, FTR_TY);
}

void lcd_show_status(bool wifi_ok, bool ap_mode, const String& ip,
                     bool sd_ok, float sd_free_gb, float sd_total_gb) {
    // Read IP mode config from NVS — cached after first read so NVS is only
    // accessed when the mode actually changes, not every 15-second refresh.
    static uint8_t s_last_ip_mode = 255;
    static String  s_mask_cache;
    static String  s_gw_cache;
    static bool    s_last_ap = false;

    // ip_mode / mask / gw only change after a WiFi-save restart — cache after first read,
    // never touch NVS again until lcd_invalidate_layout() resets s_last_ip_mode to 255.
    uint8_t ip_mode;
    if (s_last_ip_mode == 255) {
        Preferences wP;
        wP.begin("wifi", true);
        ip_mode = wP.getUChar("ip_mode", 0);
        if (ip_mode == 1) {
            s_mask_cache = wP.getString("s_mask", "255.255.255.0");
            s_gw_cache   = wP.getString("s_gw",   "");
        } else {
            s_mask_cache = "";
            s_gw_cache   = "";
        }
        wP.end();
    } else {
        ip_mode = s_last_ip_mode;  // cached — no NVS I/O
    }
    const String& s_mask = s_mask_cache;
    const String& s_gw   = s_gw_cache;

    // Invalidate cached layout when AP↔STA or ip_mode changes.
    if (ap_mode != s_last_ap || ip_mode != s_last_ip_mode) {
        s_layout_drawn = false;
        s_last_ap      = ap_mode;
        s_last_ip_mode = ip_mode;
    }

    // Value cache — skip SPI writes when nothing has changed since last refresh.
    static bool   s_last_wifi_ok  = false;
    static bool   s_last_sd_ok    = false;
    static float  s_last_free_gb  = -1.0f;
    static float  s_last_total_gb = -1.0f;
    static String s_last_ip;
    bool values_changed = s_layout_drawn == false  // layout invalidated → force
                       || wifi_ok    != s_last_wifi_ok
                       || sd_ok      != s_last_sd_ok
                       || sd_free_gb != s_last_free_gb
                       || ip         != s_last_ip;

    // First call (or after invalidation): draw static chrome + labels.
    if (!s_layout_drawn) {
        draw_header("NETWORK MODE");
        clear_content();
        tft.setTextSize(1);
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(C_DIM, C_BG);
        if (ap_mode) {
            tft.drawString("AP:",   3, 22);
            tft.drawString("Pass:", 3, 52);
            tft.drawString("SD:",   3, 100);
        } else if (ip_mode == 1) {
            // Static IP labels
            tft.drawString("IP:",      3, 22);
            tft.drawString("Mask:",    3, 47);
            tft.drawString("Gateway:", 3, 72);
            tft.drawString("SD:",      3, 97);
        } else {
            // DHCP labels
            tft.drawString("WiFi:",     3, 22);
            tft.drawString("Automatic", 3, 48);
            tft.drawString("SD:",       3, 62);
        }
        draw_footer("BOOT: switch");
        s_layout_drawn = true;
    }

    if (!values_changed) return;   // nothing to repaint — skip all SPI writes

    tft.setTextSize(1);
    tft.setTextDatum(ML_DATUM);

    if (ap_mode) {
        // ── AP SSID (y=34) ──────────────────────────────────
        tft.fillRect(0, 29, LCD_W, 13, C_BG);
        tft.setTextColor(C_WARN, C_BG);
        tft.drawString(AP_SSID, 3, 34);

        // ── AP password (y=64) ──────────────────────────────
        tft.fillRect(0, 59, LCD_W, 13, C_BG);
        tft.setTextColor(C_WARN, C_BG);
        tft.drawString(AP_PASS, 3, 64);

        // ── AP IP address (y=82) ────────────────────────────
        tft.fillRect(0, 77, LCD_W, 13, C_BG);
        tft.setTextColor(C_ACCENT, C_BG);
        tft.drawString(ip.c_str(), 3, 82);

        // ── SD value (y=112) ────────────────────────────────
        tft.fillRect(0, 107, LCD_W, 13, C_BG);
        if (sd_ok) {
            char buf[18];
            snprintf(buf, sizeof(buf), "%.1f/%.0fGB", sd_free_gb, sd_total_gb);
            tft.setTextColor(C_OK, C_BG);
            tft.drawString(buf, 3, 112);
        } else {
            tft.setTextColor(C_ERROR, C_BG);
            tft.drawString("Not found", 3, 112);
        }
    } else if (ip_mode == 1) {
        // ── Static IP: IP value (y=34) ──────────────────────
        tft.fillRect(0, 29, LCD_W, 13, C_BG);
        tft.setTextColor(wifi_ok ? C_OK : C_ERROR, C_BG);
        tft.drawString(wifi_ok ? ip.c_str() : "Not connected", 3, 34);

        // ── Mask value (y=59) ───────────────────────────────
        tft.fillRect(0, 54, LCD_W, 13, C_BG);
        tft.setTextColor(C_ACCENT, C_BG);
        tft.drawString(s_mask.c_str(), 3, 59);

        // ── Gateway value (y=84) ────────────────────────────
        tft.fillRect(0, 79, LCD_W, 13, C_BG);
        tft.setTextColor(C_ACCENT, C_BG);
        tft.drawString(s_gw.c_str(), 3, 84);

        // ── SD value (y=109) ────────────────────────────────
        tft.fillRect(0, 104, LCD_W, 13, C_BG);
        if (sd_ok) {
            char buf[18];
            snprintf(buf, sizeof(buf), "%.1f/%.0fGB", sd_free_gb, sd_total_gb);
            tft.setTextColor(C_OK, C_BG);
            tft.drawString(buf, 3, 109);
        } else {
            tft.setTextColor(C_ERROR, C_BG);
            tft.drawString("Not found", 3, 109);
        }
    } else {
        // ── DHCP: WiFi IP value (y=34) ──────────────────────
        tft.fillRect(0, 29, LCD_W, 13, C_BG);
        tft.setTextColor(wifi_ok ? C_OK : C_ERROR, C_BG);
        tft.drawString(wifi_ok ? ip.c_str() : "Not connected", 3, 34);

        // ── SD value (y=74) ─────────────────────────────────
        tft.fillRect(0, 69, LCD_W, 13, C_BG);
        if (sd_ok) {
            char buf[20];
            snprintf(buf, sizeof(buf), "%.1f / %.0f GB", sd_free_gb, sd_total_gb);
            tft.setTextColor(C_OK, C_BG);
            tft.drawString(buf, 3, 74);
        } else {
            tft.setTextColor(C_ERROR, C_BG);
            tft.drawString("Not found", 3, 74);
        }
    }

    // Update value cache
    s_last_wifi_ok  = wifi_ok;
    s_last_sd_ok    = sd_ok;
    s_last_free_gb  = sd_free_gb;
    s_last_total_gb = sd_total_gb;
    s_last_ip       = ip;
}

// Format "X.X/Y.YMB" or "X/YKB" or "X.XMB" (unknown total).  buf must be ≥18 bytes.
static void fmt_size(char* buf, int len, int32_t recv, int total) {
    if (total > 0) {
        if (total >= 1073741824L)
            snprintf(buf, len, "%.1f/%.1fGB", recv / 1073741824.0f, total / 1073741824.0f);
        else if (total >= 1048576)
            snprintf(buf, len, "%.1f/%.1fMB", recv / 1048576.0f, total / 1048576.0f);
        else
            snprintf(buf, len, "%d/%dKB", (int)(recv / 1024), (int)(total / 1024));
    } else {
        if (recv >= 1048576)
            snprintf(buf, len, "%.1f MB", recv / 1048576.0f);
        else
            snprintf(buf, len, "%d KB", (int)(recv / 1024));
    }
}

// Format "ZZZ KB/s" or "Z.Z MB/s" or "--" when not yet sampled.
static void fmt_speed(char* buf, int len, int kbps) {
    if (kbps < 0)         snprintf(buf, len, "--");
    else if (kbps >= 1024) snprintf(buf, len, "%.1f MB/s", kbps / 1024.0f);
    else                   snprintf(buf, len, "%d KB/s", kbps);
}

// Format remaining time: "ETA: 2m30s", "ETA: <1s", or "ETA: --".
static void fmt_eta(char* buf, int len, int speed_kbps, int32_t recv, int total) {
    if (speed_kbps <= 0 || total <= 0 || recv >= total) {
        snprintf(buf, len, "ETA: --");
        return;
    }
    int32_t remaining = total - recv;
    int secs = (int)((int64_t)remaining / ((int64_t)speed_kbps * 1024));
    if (secs < 1)       snprintf(buf, len, "ETA: <1s");
    else if (secs < 60) snprintf(buf, len, "ETA: %ds", secs);
    else                snprintf(buf, len, "ETA: %dm%ds", secs / 60, secs % 60);
}

void lcd_show_progress(const String& label, uint8_t percent,
                       int speed_kbps, int32_t bytes_recv, int content_len,
                       int queue_remaining, const char* header) {
    const uint8_t BAR_X = 10;
    const uint8_t BAR_Y = 70;
    const uint8_t BAR_W = LCD_W - 20;
    const uint8_t BAR_H = 12;

    bool label_changed = (strncmp(label.c_str(), s_prog_last_lbl, sizeof(s_prog_last_lbl) - 1) != 0);
    bool first_draw    = (s_prog_last_pct == 255) || label_changed;

    // ── Build header string (shared by first_draw and queue-change redraw) ───────
    auto make_hdr = [&](char* buf, size_t bufsz) {
        if (header != nullptr)        strncpy(buf, header, bufsz - 1), buf[bufsz-1] = '\0';
        else if (queue_remaining > 0) snprintf(buf, bufsz, "DL +%d queued", queue_remaining);
        else                          strncpy(buf, "Downloading", bufsz);
    };

    if (first_draw) {
        // Full redraw: chrome + static elements
        // "DL +999 queued\0" = 16 bytes — assert ensures hdr[20] fits up to 3-digit queue counts.
        static_assert(sizeof("DL +999 queued") <= 20, "hdr[] too small for max queue count");
        char hdr[20]; make_hdr(hdr, sizeof(hdr));
        draw_header(hdr);
        s_prog_last_queue = queue_remaining;
        strncpy(s_prog_last_hdr, hdr, sizeof(s_prog_last_hdr) - 1);
        s_prog_last_hdr[sizeof(s_prog_last_hdr) - 1] = '\0';
        clear_content();
        tft.setTextDatum(MC_DATUM);
        tft.setTextSize(1);
        tft.setTextColor(C_TEXT, C_BG);
        tft.drawString(label, LCD_W / 2, 50);
        tft.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, C_DIM);
        strncpy(s_prog_last_lbl, label.c_str(), sizeof(s_prog_last_lbl) - 1);
        s_prog_last_lbl[sizeof(s_prog_last_lbl) - 1] = '\0';
        s_prog_last_pct = 255;   // force bar + text update below
    }

    // Redraw header when queue count or header string changes (after first_draw handled it above)
    if (!first_draw) {
        char hdr[20]; make_hdr(hdr, sizeof(hdr));
        if (queue_remaining != s_prog_last_queue ||
            strncmp(hdr, s_prog_last_hdr, sizeof(s_prog_last_hdr)) != 0) {
            draw_header(hdr);
            s_prog_last_queue = queue_remaining;
            strncpy(s_prog_last_hdr, hdr, sizeof(s_prog_last_hdr) - 1);
            s_prog_last_hdr[sizeof(s_prog_last_hdr) - 1] = '\0';
        }
    }

    if (percent != s_prog_last_pct) {
        // Partial redraw: bar fill + percentage only
        tft.fillRect(BAR_X + 1, BAR_Y + 1, BAR_W - 2, BAR_H - 2, C_BG);
        if (percent > 0) {
            uint16_t fill = (uint16_t)(BAR_W * percent / 100);
            tft.fillRect(BAR_X + 1, BAR_Y + 1, fill, BAR_H - 2, C_BAR);
        }
        char buf[6];
        snprintf(buf, sizeof(buf), "%d%%", percent);
        tft.fillRect(0, 87, LCD_W, 13, C_BG);
        tft.setTextDatum(MC_DATUM);
        tft.setTextSize(1);
        tft.setTextColor(C_TEXT, C_BG);
        tft.drawString(buf, LCD_W / 2, 95);
        s_prog_last_pct = percent;
    }

    // Speed / size / ETA rows — update only the rows whose string representation changed.
    // Each row costs 2 SPI transactions (fillRect erase + drawString); skipping unchanged
    // rows halves typical SPI overhead during a download (size changes infrequently).
    bool stats_changed = first_draw ||
                         (speed_kbps != s_prog_last_spd) ||
                         (bytes_recv != s_prog_last_byt);
    if (stats_changed) {
        char sbuf[18];
        tft.setTextDatum(MC_DATUM);
        tft.setTextSize(1);

        // Row 1 (y=100..112): received / total size
        fmt_size(sbuf, sizeof(sbuf), bytes_recv, content_len);
        if (strncmp(sbuf, s_prog_last_size, sizeof(sbuf)) != 0) {
            tft.fillRect(0, 100, LCD_W, 13, C_BG);
            tft.setTextColor(C_TEXT, C_BG);
            tft.drawString(sbuf, LCD_W / 2, 107);
            strncpy(s_prog_last_size, sbuf, sizeof(s_prog_last_size) - 1);
            s_prog_last_size[sizeof(s_prog_last_size) - 1] = '\0';
        }

        // Row 2 (y=113..125): download speed
        fmt_speed(sbuf, sizeof(sbuf), speed_kbps);
        if (strncmp(sbuf, s_prog_last_spds, sizeof(sbuf)) != 0) {
            tft.fillRect(0, 113, LCD_W, 13, C_BG);
            tft.setTextColor(C_ACCENT, C_BG);
            tft.drawString(sbuf, LCD_W / 2, 120);
            strncpy(s_prog_last_spds, sbuf, sizeof(s_prog_last_spds) - 1);
            s_prog_last_spds[sizeof(s_prog_last_spds) - 1] = '\0';
        }

        // Row 3 (y=126..138): remaining time (only when size is known)
        fmt_eta(sbuf, sizeof(sbuf), speed_kbps, bytes_recv, content_len);
        if (strncmp(sbuf, s_prog_last_eta, sizeof(sbuf)) != 0) {
            tft.fillRect(0, 126, LCD_W, 13, C_BG);
            tft.setTextColor(C_WARN, C_BG);
            tft.drawString(sbuf, LCD_W / 2, 133);
            strncpy(s_prog_last_eta, sbuf, sizeof(s_prog_last_eta) - 1);
            s_prog_last_eta[sizeof(s_prog_last_eta) - 1] = '\0';
        }

        s_prog_last_spd = speed_kbps;
        s_prog_last_byt = bytes_recv;
    }
}


void lcd_show_usb_mode(bool sd_ok, float sd_free_gb, float sd_total_gb) {
    draw_header("USB DRIVE");
    clear_content();

    tft.setTextSize(1);
    tft.setTextDatum(ML_DATUM);

    // SD Card label + status
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString("SD Card:", 3, 22);

    tft.setTextColor(sd_ok ? C_OK : C_ERROR, C_BG);
    tft.drawString(sd_ok ? "Drive active" : "Not found", 3, 34);

    // SD size (only when card is present)
    if (sd_ok) {
        char buf[20];
        snprintf(buf, sizeof(buf), "%.1f / %.0f GB", sd_free_gb, sd_total_gb);
        tft.setTextColor(C_OK, C_BG);
        tft.drawString(buf, 3, 46);
    }

    tft.setTextColor(C_DIM, C_BG);
    tft.drawString("WiFi: OFF", 3, 68);

    tft.setTextColor(C_WARN, C_BG);
    tft.drawString(".vbs or BOOT:", 3, 82);
    tft.drawString("switch to net", 3, 94);

    draw_footer("BOOT: switch");
}

void lcd_invalidate_layout() {
    s_layout_drawn    = false;
    s_prog_last_pct   = 255;    // force full redraw on next lcd_show_progress()
    s_prog_last_lbl[0] = '\0';
    s_prog_last_spd   = -2;
    s_prog_last_byt   = -1;
    s_prog_last_queue = -1;
    s_prog_last_hdr[0]  = '\0';
    s_prog_last_size[0] = '\0';
    s_prog_last_spds[0] = '\0';
    s_prog_last_eta[0]  = '\0';
}

void lcd_set_backlight(uint8_t brightness) {
    // Phase 5 will replace this with ledcWrite PWM.
    // For now: any non-zero value = on.
    digitalWrite(PIN_LCD_BL, brightness > 0 ? LOW : HIGH);
}

void lcd_show_qr(const char* url) {
    // ricmoo/QRCode library — qrcode_getBufferSize(v) = ceil((modules^2)/8).
    // Version 2 (25 modules) holds up to 32 bytes in ECC_LOW byte mode — enough
    // for any LAN IP ("http://192.168.xxx.xxx" = 22 chars).
    // Version 3 (29 modules) holds up to 53 bytes — fallback for longer URLs.
    // Fixed 200-byte buffer covers Version 2 (79 B) and Version 3 (106 B).
    static uint8_t qr_buf[200];
    QRCode qr;

    // Try Version 2 first; fall back to Version 3 for longer URLs.
    uint8_t ver = 2;
    if (qrcode_initText(&qr, qr_buf, ver, ECC_LOW, url) != 0) {
        ver = 3;
        if (200 < (int)qrcode_getBufferSize(ver) ||
            qrcode_initText(&qr, qr_buf, ver, ECC_LOW, url) != 0) {
            Serial.println("[QR] URL too long for Version 3 — skipping QR display");
            return;
        }
    }

    // Pixel size per module: Version 2 → 25 modules, 3 px/module = 75 px (fits in 80).
    //                        Version 3 → 29 modules, 2 px/module = 58 px.
    uint8_t mod_px = (ver == 2) ? 3 : 2;
    uint8_t qr_px  = qr.size * mod_px;                 // total QR pixel dimension
    uint8_t x_off  = (uint8_t)((LCD_W - qr_px) / 2);  // horizontal centering
    uint8_t y_off  = 18;                                // 4 px below header (HDR_H=14)

    // Draw white canvas over content area (QR needs high contrast, theme-agnostic).
    tft.fillRect(0, HDR_H, LCD_W, FTR_Y - HDR_H, 0xFFFF);

    // Render QR modules: dark=black (0x0000), light=white (0xFFFF).
    for (uint8_t qy = 0; qy < qr.size; qy++) {
        for (uint8_t qx = 0; qx < qr.size; qx++) {
            uint16_t col = qrcode_getModule(&qr, qx, qy) ? 0x0000u : 0xFFFFu;
            tft.fillRect(x_off + qx * mod_px, y_off + qy * mod_px,
                         mod_px, mod_px, col);
        }
    }

    // "Scan to open" caption below the QR matrix.
    uint8_t cap_y = y_off + qr_px + 7;
    tft.setTextColor(0x0000u, 0xFFFFu);   // black on white
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);
    tft.drawString("Scan to open", LCD_W / 2, cap_y);

    // URL line (truncated to 13 chars if needed — LCD_W=80 px, size-1 = ~6px/char).
    char url_short[14];
    strncpy(url_short, url, 13);
    url_short[13] = '\0';
    tft.setTextColor(0x001Fu, 0xFFFFu);   // dark blue on white
    tft.drawString(url_short, LCD_W / 2, cap_y + 14);

    // Footer: keep theme colours so the hint is visible.
    tft.drawFastHLine(0, FTR_Y, LCD_W, C_DIM);
    tft.setTextColor(C_DIM, 0xFFFFu);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("BOOT: switch", 3, FTR_TY);

    // Preserve the QR header bar (re-use existing draw_header).
    draw_header("SCAN TO CONNECT");
}
