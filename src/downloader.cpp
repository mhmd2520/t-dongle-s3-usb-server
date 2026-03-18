#include "downloader.h"
#include "storage.h"
#include "lcd.h"
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ── Constants ─────────────────────────────────────────────────────────────────
#define DL_URL_MAX   512
#define DL_FNAME_MAX  63
#define DL_BUF_SIZE 32768    // 32 KB transfer buffer — reduces SD write calls and loop overhead
#define DL_LCD_EVERY 32768   // update LCD every 32 KB

// ── State (volatile fields are read by Core 0 via API handlers) ───────────────
static volatile bool  s_dl_pending  = false;   // Core 0 sets, Core 1 clears
static char           s_dl_url[DL_URL_MAX + 1];
static volatile bool  s_dl_active   = false;
static volatile bool  s_dl_cancel   = false;   // Core 0 sets → Core 1 aborts stream loop
static volatile int   s_dl_progress = 0;       // 0-100 or -1
static char           s_dl_filename[DL_FNAME_MAX + 1];
static char           s_dl_status[80];
static uint8_t        s_buf[DL_BUF_SIZE];

// ── Private helpers ───────────────────────────────────────────────────────────

// Decode a single %XX hex escape; returns decoded char or 0 on bad input.
static char hex_decode(char hi, char lo) {
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int h = hv(hi), l = hv(lo);
    if (h < 0 || l < 0) return 0;
    return (char)((h << 4) | l);
}

// Return true if c is illegal in a FAT32 filename.
static bool fat32_illegal(char c) {
    if ((uint8_t)c < 0x20) return true;
    const char* bad = "\\/:*?\"<>|";
    while (*bad) if (*bad++ == c) return true;
    return false;
}

// Parse filename from URL into out (DL_FNAME_MAX+1 bytes).
static void parse_filename(const char* url, char* out) {
    // Skip scheme (http:// or https://)
    const char* p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://",  7) == 0) p += 7;

    // Skip host — find first '/' after host
    const char* slash = strchr(p, '/');
    if (!slash) { strncpy(out, "download", DL_FNAME_MAX); out[DL_FNAME_MAX] = '\0'; return; }

    // Find last '/' to get the final path segment
    const char* seg = strrchr(slash, '/');
    seg = seg ? seg + 1 : slash + 1;

    // Copy segment, strip at first '?' or '#'
    char tmp[DL_FNAME_MAX + 1] = {};
    int ti = 0;
    while (*seg && *seg != '?' && *seg != '#' && ti < DL_FNAME_MAX) {
        if (*seg == '%' && seg[1] && seg[2]) {
            char d = hex_decode(seg[1], seg[2]);
            if (d && !fat32_illegal(d)) tmp[ti++] = d;
            else tmp[ti++] = '_';
            seg += 3;
        } else {
            tmp[ti++] = fat32_illegal(*seg) ? '_' : *seg;
            seg++;
        }
    }
    tmp[ti] = '\0';

    if (ti == 0) strncpy(tmp, "download", DL_FNAME_MAX);

    // Collision avoidance on SD
    String base = "/" + String(tmp);
    if (!SD_MMC.exists(base.c_str())) {
        strncpy(out, tmp, DL_FNAME_MAX); out[DL_FNAME_MAX] = '\0';
        return;
    }
    // Split into stem + ext
    String stem = tmp, ext = "";
    int dot = String(tmp).lastIndexOf('.');
    if (dot > 0) { stem = String(tmp).substring(0, dot); ext = String(tmp).substring(dot); }
    for (int i = 2; i <= 99; i++) {
        String candidate = "/" + stem + "_" + i + ext;
        if (!SD_MMC.exists(candidate.c_str())) {
            String fn = stem + "_" + i + ext;
            strncpy(out, fn.c_str(), DL_FNAME_MAX); out[DL_FNAME_MAX] = '\0';
            return;
        }
    }
    // All taken — overwrite _99
    String fn = stem + "_99" + ext;
    strncpy(out, fn.c_str(), DL_FNAME_MAX); out[DL_FNAME_MAX] = '\0';
}

// Rough filename from URL for immediate API response (no SD check needed).
static String quick_filename(const char* url) {
    String s = url;
    int q = s.indexOf('?'); if (q >= 0) s = s.substring(0, q);
    int h = s.indexOf('#'); if (h >= 0) s = s.substring(0, h);
    int sl = s.lastIndexOf('/'); if (sl >= 0) s = s.substring(sl + 1);
    if (s.isEmpty()) s = "download";
    if ((int)s.length() > DL_FNAME_MAX) s = s.substring(0, DL_FNAME_MAX);
    return s;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool downloader_queue(const char* url) {
    if (s_dl_active || s_dl_pending) return false;
    strncpy(s_dl_url, url, DL_URL_MAX);
    s_dl_url[DL_URL_MAX] = '\0';
    __asm__ volatile ("" ::: "memory");   // compiler barrier — write data before flag
    s_dl_pending = true;
    return true;
}

bool downloader_is_busy() {
    return s_dl_active || s_dl_pending;
}

void downloader_cancel() {
    if (s_dl_active || s_dl_pending) {
        s_dl_pending = false;   // drop queued-but-not-started download too
        s_dl_cancel  = true;
    }
}

int downloader_progress() {
    return (int)s_dl_progress;
}

const char* downloader_filename() {
    return s_dl_filename;
}

const char* downloader_status() {
    return s_dl_status;
}

// ── Main download state machine (Core 1 only) ─────────────────────────────────

void downloader_run() {
    if (!s_dl_pending) return;

    // Claim the work
    s_dl_pending = false;
    if (s_dl_active) return;   // safety guard
    s_dl_active   = true;
    s_dl_progress = 0;
    snprintf(s_dl_status, sizeof(s_dl_status), "downloading");

    // Pre-flight: SD ready
    if (!storage_is_ready()) {
        snprintf(s_dl_status, sizeof(s_dl_status), "error: SD not ready");
        s_dl_active = false;
        return;
    }

    // Pre-flight: free space (< 1 MB → reject)
    if (storage_free_gb() < 0.000954f) {   // 0.000954 GB ≈ 1 MB
        snprintf(s_dl_status, sizeof(s_dl_status), "error: SD full");
        s_dl_active = false;
        return;
    }

    // Parse filename (with collision avoidance)
    parse_filename(s_dl_url, s_dl_filename);

    // First LCD paint
    lcd_show_progress(s_dl_filename, 0);

    // ── Open HTTP connection ───────────────────────────────────────────────────
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(30000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);

    WiFiClientSecure* sc = nullptr;
    bool is_https = strncmp(s_dl_url, "https", 5) == 0;
    if (is_https) {
        sc = new WiFiClientSecure();
        sc->setInsecure();
        http.begin(*sc, s_dl_url);
    } else {
        http.begin(s_dl_url);
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        snprintf(s_dl_status, sizeof(s_dl_status), "error: HTTP %d", code);
        http.end();
        if (sc) { delete sc; sc = nullptr; }
        s_dl_active = false;
        lcd_invalidate_layout();
        return;
    }

    int content_len = http.getSize();
    bool known_size = (content_len > 0);

    // ── Open SD file ──────────────────────────────────────────────────────────
    String sdpath = "/" + String(s_dl_filename);
    File outf = SD_MMC.open(sdpath.c_str(), FILE_WRITE);
    if (!outf) {
        snprintf(s_dl_status, sizeof(s_dl_status), "error: SD open failed");
        http.end();
        if (sc) { delete sc; sc = nullptr; }
        s_dl_active = false;
        lcd_invalidate_layout();
        return;
    }

    // ── Stream loop ───────────────────────────────────────────────────────────
    WiFiClient* stream = http.getStreamPtr();
    int32_t bytes_recv = 0;
    int32_t last_lcd   = -DL_LCD_EVERY;   // force first update immediately
    bool write_err  = false;
    bool cancelled  = false;

    while (http.connected() || stream->available()) {
        // Cancel check — set by Core 0 via downloader_cancel()
        if (s_dl_cancel) { cancelled = true; break; }

        int avail = stream->available();
        if (avail > 0) {
            int chunk = (avail > DL_BUF_SIZE) ? DL_BUF_SIZE : avail;
            int n = stream->readBytes(s_buf, chunk);
            if (n > 0) {
                size_t written = outf.write(s_buf, (size_t)n);
                if (written != (size_t)n) { write_err = true; break; }
                bytes_recv += n;

                // Update progress state
                if (known_size) {
                    s_dl_progress = (int)((int64_t)bytes_recv * 100 / content_len);
                    snprintf(s_dl_status, sizeof(s_dl_status),
                             "downloading (%d%%)", (int)s_dl_progress);
                } else {
                    s_dl_progress = -1;
                    snprintf(s_dl_status, sizeof(s_dl_status),
                             "downloading (%d KB)", (int)(bytes_recv / 1024));
                }

                // LCD update every DL_LCD_EVERY bytes
                if (bytes_recv - last_lcd >= DL_LCD_EVERY) {
                    last_lcd = bytes_recv;
                    if (known_size) {
                        lcd_show_progress(s_dl_filename, (uint8_t)s_dl_progress);
                    } else {
                        char lbl[80];
                        snprintf(lbl, sizeof(lbl), "%s\n%d KB",
                                 s_dl_filename, (int)(bytes_recv / 1024));
                        lcd_show_progress(lbl, 0);
                    }
                }
            }
        } else {
            // No bytes available yet — check if we're done
            if (known_size && bytes_recv >= (int32_t)content_len) break;
            // No delay — yield() alone is sufficient to feed the watchdog
        }
        yield();   // feed watchdog, let Core 0 handle HTTP requests
    }
    outf.close();

    // Limit TLS shutdown wait to 1 s — prevents task watchdog reset on slow servers.
    if (sc) sc->setTimeout(1000);
    http.end();
    if (sc) { delete sc; sc = nullptr; }

    // ── Cancelled ─────────────────────────────────────────────────────────────
    if (cancelled) {
        SD_MMC.remove(sdpath.c_str());
        s_dl_cancel = false;
        snprintf(s_dl_status, sizeof(s_dl_status), "cancelled");
        s_dl_active = false;
        lcd_invalidate_layout();
        Serial.printf("[DL] Cancelled: /%s\n", s_dl_filename);
        return;
    }

    // ── Error: SD write failure ───────────────────────────────────────────────
    if (write_err) {
        SD_MMC.remove(sdpath.c_str());
        snprintf(s_dl_status, sizeof(s_dl_status), "error: write failed");
        s_dl_active = false;
        lcd_invalidate_layout();
        return;
    }

    // ── Error: truncated response ─────────────────────────────────────────────
    if (known_size && bytes_recv < (int32_t)content_len) {
        SD_MMC.remove(sdpath.c_str());
        snprintf(s_dl_status, sizeof(s_dl_status),
                 "error: incomplete (%d/%d B)", (int)bytes_recv, content_len);
        s_dl_active = false;
        lcd_invalidate_layout();
        return;
    }

    // ── Success ───────────────────────────────────────────────────────────────
    s_dl_progress = 100;
    snprintf(s_dl_status, sizeof(s_dl_status), "done");
    lcd_show_progress(s_dl_filename, 100);
    delay(800);
    lcd_invalidate_layout();
    s_dl_active = false;

    Serial.printf("[DL] Done: /%s (%d bytes)\n", s_dl_filename, (int)bytes_recv);
}

// Expose quick_filename for web_server response (avoids duplicating logic there)
String downloader_quick_filename(const char* url) {
    return quick_filename(url);
}
