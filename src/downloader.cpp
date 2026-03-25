#include "downloader.h"
#include "actlog.h"
#include "storage.h"
#include "lcd.h"
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <NetworkClient.h>     // Core 3.x: getStreamPtr() returns NetworkClient*
#include <lwip/sockets.h>    // SO_RCVBUF — increases advertised TCP receive window at runtime
#include <freertos/semphr.h>

// ── Constants ─────────────────────────────────────────────────────────────────
#define DL_URL_MAX    512
#define DL_FNAME_MAX   63
#define DL_HALF_SIZE  32768  // each ping-pong buffer half (32 KB)
#define DL_BUF_SIZE   65536  // total buffer: two DL_HALF_SIZE halves for double-buffering
#define DL_LCD_EVERY    16384  // update LCD every 16 KB received
#define DL_QUEUE_SIZE  10    // max URLs waiting in queue

// ── SD write task — runs on Core 1 alongside Arduino loop ─────────────────────
// During SDMMC DMA, the task blocks on a FreeRTOS semaphore (CPU released),
// allowing the Arduino loop to keep draining the TCP socket concurrently.
static SemaphoreHandle_t s_wr_start = nullptr;  // main → task: data ready to write
static SemaphoreHandle_t s_wr_done  = nullptr;  // task → main: write complete
static File*             s_wr_file  = nullptr;
static uint8_t*          s_wr_ptr   = nullptr;  // nullptr = terminate
static size_t            s_wr_len   = 0;
static bool              s_wr_err   = false;

static void sd_writer_task(void*) {
    while (true) {
        xSemaphoreTake(s_wr_start, portMAX_DELAY);
        if (!s_wr_ptr) break;                    // nullptr = graceful exit
        size_t w = s_wr_file->write(s_wr_ptr, s_wr_len);
        s_wr_err = (w != s_wr_len);
        xSemaphoreGive(s_wr_done);
    }
    vTaskDelete(nullptr);
}

// ── Download queue — ring buffer, protected by spinlock (Core 0 writes, Core 1 reads) ──
static portMUX_TYPE     s_q_mux     = portMUX_INITIALIZER_UNLOCKED;
static char             s_dl_queue[DL_QUEUE_SIZE][DL_URL_MAX + 1];
static int              s_dl_q_head = 0;             // next slot to dequeue (Core 1)
static int              s_dl_q_tail = 0;             // next slot to enqueue (Core 0)
static volatile int     s_dl_q_count = 0;            // items waiting (read from both cores)

// Working URL for the download currently in progress (Core 1 only)
static char             s_dl_url[DL_URL_MAX + 1];

// ── State (volatile fields are read by Core 0 via API handlers) ───────────────
static volatile bool    s_dl_active        = false;
static volatile bool    s_dl_cancel        = false;   // Core 0 sets → Core 1 aborts
static volatile uint8_t s_dl_conflict      = 0;       // 0=none,1=waiting,2=replace,3=skip,4=cancel
static char             s_dl_conflict_name[DL_FNAME_MAX + 1];
static volatile int     s_dl_progress      = 0;       // 0-100 or -1
static char             s_dl_filename[DL_FNAME_MAX + 1];
static char             s_dl_status[80];
static uint8_t          s_buf[DL_BUF_SIZE];           // ping-pong: [0..32767] and [32768..65535]
static volatile int     s_dl_speed_kbps    = 0;       // KB/s, updated every ~1 s
static volatile int32_t s_dl_bytes_recv    = 0;       // bytes received so far
static volatile int     s_dl_content_len   = -1;      // server Content-Length (-1 if unknown)

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

    strncpy(out, tmp, DL_FNAME_MAX); out[DL_FNAME_MAX] = '\0';
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
    bool ok = false;
    taskENTER_CRITICAL(&s_q_mux);
    if (s_dl_q_count < DL_QUEUE_SIZE) {
        strncpy(s_dl_queue[s_dl_q_tail], url, DL_URL_MAX);
        s_dl_queue[s_dl_q_tail][DL_URL_MAX] = '\0';
        s_dl_q_tail = (s_dl_q_tail + 1) % DL_QUEUE_SIZE;
        s_dl_q_count = s_dl_q_count + 1;
        ok = true;
    }
    taskEXIT_CRITICAL(&s_q_mux);
    return ok;
}

int downloader_queue_count() {
    // Read inside critical section — Xtensa 32-bit aligned reads are atomic
    // in practice, but the critical section prevents the compiler from splitting
    // the read across two instructions on future compiler versions.
    taskENTER_CRITICAL(&s_q_mux);
    int n = s_dl_q_count;
    taskEXIT_CRITICAL(&s_q_mux);
    return n;
}

bool downloader_is_busy() {
    taskENTER_CRITICAL(&s_q_mux);
    bool busy = s_dl_active || s_dl_q_count > 0;
    taskEXIT_CRITICAL(&s_q_mux);
    return busy;
}

void downloader_cancel() {
    // Clear the entire queue and cancel the active download.
    taskENTER_CRITICAL(&s_q_mux);
    s_dl_q_count = 0;
    s_dl_q_head  = 0;
    s_dl_q_tail  = 0;
    bool was_active = s_dl_active;   // read under same lock as queue state — prevents TOCTOU
    // Set cancel flags while still inside the lock so Core 1 cannot dequeue a new URL
    // in the window between EXIT_CRITICAL and the stores (TOCTOU fix).
    if (was_active) s_dl_cancel = true;
    if (s_dl_conflict == 1) s_dl_conflict = 4;  // unblock conflict wait
    taskEXIT_CRITICAL(&s_q_mux);
}

bool downloader_conflict_pending() {
    return s_dl_conflict == 1;
}

const char* downloader_conflict_name() {
    return s_dl_conflict_name;
}

void downloader_resolve(const char* action) {
    if (s_dl_conflict != 1) return;
    if      (strcmp(action, "replace") == 0) s_dl_conflict = 2;
    else if (strcmp(action, "skip")    == 0) s_dl_conflict = 3;
    else                                     s_dl_conflict = 4;  // cancel
}

int downloader_progress() {
    return (int)s_dl_progress;
}

int downloader_speed() {
    return (int)s_dl_speed_kbps;
}

int32_t downloader_bytes_recv() {
    return s_dl_bytes_recv;
}

int downloader_content_len() {
    return (int)s_dl_content_len;
}

const char* downloader_filename() {
    return s_dl_filename;
}

const char* downloader_status() {
    return s_dl_status;
}

// ── Main download state machine (Core 1 only) ─────────────────────────────────

void downloader_run() {
    if (s_dl_active) return;   // Core 1 is sole writer of s_dl_active — safe outside lock

    // Dequeue next URL atomically; set s_dl_active inside the lock so
    // downloader_cancel() and downloader_is_busy() see a consistent (active, count) pair.
    taskENTER_CRITICAL(&s_q_mux);
    if (s_dl_q_count == 0) { taskEXIT_CRITICAL(&s_q_mux); return; }
    strncpy(s_dl_url, s_dl_queue[s_dl_q_head], DL_URL_MAX);
    s_dl_url[DL_URL_MAX] = '\0';
    s_dl_q_head  = (s_dl_q_head + 1) % DL_QUEUE_SIZE;
    s_dl_q_count = s_dl_q_count - 1;
    s_dl_active  = true;   // set while locked — atomic view from Core 0
    taskEXIT_CRITICAL(&s_q_mux);

    s_dl_progress      = 0;
    s_dl_speed_kbps    = 0;
    s_dl_bytes_recv    = 0;
    s_dl_content_len   = -1;
    snprintf(s_dl_status, sizeof(s_dl_status), "downloading");

    // Pre-flight: SD ready
    if (!storage_is_ready()) {
        snprintf(s_dl_status, sizeof(s_dl_status), "error: SD not ready");
        s_dl_cancel = false;
        s_dl_active = false;
        return;
    }

    // Pre-flight: free space (< 1 MB → reject)
    if (storage_free_gb() < 0.000954f) {   // 0.000954 GB ≈ 1 MB
        snprintf(s_dl_status, sizeof(s_dl_status), "error: SD full");
        s_dl_cancel = false;
        s_dl_active = false;
        return;
    }

    // Parse filename from URL
    parse_filename(s_dl_url, s_dl_filename);

    // sdpath used throughout — declare once here
    String sdpath     = "/" + String(s_dl_filename);
    bool   is_replace = false;   // true when we renamed original to .bak
    String backup_path;

    // Conflict check — if file exists, pause and ask user (Replace / Skip / Cancel)
    if (SD_MMC.exists(sdpath.c_str())) {
        strncpy(s_dl_conflict_name, s_dl_filename, sizeof(s_dl_conflict_name) - 1);
        s_dl_conflict_name[sizeof(s_dl_conflict_name) - 1] = '\0';
        snprintf(s_dl_status, sizeof(s_dl_status), "conflict");
        s_dl_conflict = 1;  // WAITING

        uint32_t deadline = millis() + 60000UL;   // 60 s timeout → auto-cancel
        while (s_dl_conflict == 1 && millis() < deadline) {
            delay(100);
            yield();
        }

        uint8_t res = s_dl_conflict;
        s_dl_conflict = 0;

        if (res == 3) {  // "Keep Both" — download to a numbered filename (_1, _2, ...)
            String fn  = String(s_dl_filename);
            int    dot = fn.lastIndexOf('.');
            String base = dot > 0 ? fn.substring(0, dot) : fn;
            String ext  = dot > 0 ? fn.substring(dot)    : String();
            bool found = false;
            for (int seq = 1; seq <= 999; seq++) {
                sdpath = "/" + base + "_" + String(seq) + ext;
                if (!SD_MMC.exists(sdpath.c_str())) { found = true; break; }
            }
            if (!found) {
                snprintf(s_dl_status, sizeof(s_dl_status), "skipped");
                s_dl_active = false;
                lcd_invalidate_layout();
                actlog_add(ACT_SKIP, s_dl_filename, "Skipped (too many copies)");
                return;
            }
            // Update active filename so LCD/status reflect the new name
            String newname = sdpath.substring(1);
            strncpy(s_dl_filename, newname.c_str(), DL_FNAME_MAX);
            s_dl_filename[DL_FNAME_MAX] = '\0';
            // fall through — download proceeds to new sdpath
        } else if (res != 2) {  // Cancel or timeout
            s_dl_cancel = false;
            snprintf(s_dl_status, sizeof(s_dl_status), "cancelled");
            s_dl_active = false;
            lcd_invalidate_layout();
            actlog_add(ACT_CANCEL, s_dl_filename, "Cancelled (conflict)");
            return;
        } else {
            // res == 2: Replace — rename original to .bak so it can be restored on failure
            backup_path = sdpath + "._bak";
            SD_MMC.remove(backup_path.c_str());   // remove stale .bak if present
            if (SD_MMC.rename(sdpath.c_str(), backup_path.c_str())) {
                is_replace = true;
            } else {
                // rename failed — fall back to direct delete
                SD_MMC.remove(sdpath.c_str());
            }
        }
    }

    // Show "Connecting..." on LCD + status while http.GET() blocks
    // (DNS resolution + TCP connect + TLS handshake can take 0.5–3 s)
    snprintf(s_dl_status, sizeof(s_dl_status), "connecting");
    lcd_show_progress(String(s_dl_filename), 0, -1, 0, -1,
                      downloader_queue_count(), "Connecting...");
    uint32_t dl_start_ms = millis();   // for avg-speed actlog detail

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
    if (code == HTTP_CODE_OK) {
        // Disable Nagle's algorithm — eliminates last-packet lag on HTTP connections.
        // SO_RCVBUF at runtime expands pcb->rcv_wnd from 5760 → 65535 bytes,
        // lifting throughput from ~30 KB/s to ~300–600 KB/s on internet links.
        NetworkClient* rawClient = http.getStreamPtr();
        if (rawClient) {
            rawClient->setNoDelay(true);
            // With CONFIG_LWIP_WND_SCALE=y + CONFIG_LWIP_TCP_RCV_SCALE=3,
            // the effective TCP window = SO_RCVBUF << 3.  Set SO_RCVBUF to
            // 65535 so the scaled window reaches 65535*8 = 524 KB, giving the
            // server up to ~1.3 s of in-flight data at 400 KB/s before stalling.
            int recv_size = 65535;
            rawClient->setSocketOption(SO_RCVBUF, (char*)&recv_size, sizeof(recv_size));
        }
    }
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
    s_dl_content_len = content_len;
    snprintf(s_dl_status, sizeof(s_dl_status), "downloading");
    // Clear "Connecting..." from LCD immediately — don't wait for DL_LCD_EVERY bytes
    lcd_show_progress(String(s_dl_filename), 0, -1, 0, content_len, downloader_queue_count());

    // ── Open SD file ──────────────────────────────────────────────────────────
    File outf = SD_MMC.open(sdpath.c_str(), FILE_WRITE);
    if (!outf) {
        snprintf(s_dl_status, sizeof(s_dl_status), "error: SD open failed");
        http.end();
        if (sc) { delete sc; sc = nullptr; }
        s_dl_active = false;
        lcd_invalidate_layout();
        return;
    }

    // ── Stream loop (double-buffered) ─────────────────────────────────────────
    // Two 32 KB ping-pong halves of s_buf[64 KB]:
    //   rd_buf — being filled from the TCP socket right now
    //   wr_buf — being written to SD by sd_writer_task (or free)
    // When rd_buf fills (32 KB), it is swapped into the write task and the
    // formerly-in-flight wr_buf becomes the new rd_buf.  Because the SDMMC
    // DMA releases Core 1 while waiting (FreeRTOS semaphore), TCP reads run
    // concurrently with SD writes — eliminating the burst/pause pattern caused
    // by the TCP window closing during synchronous SD write stalls.
    NetworkClient* stream = http.getStreamPtr();
    if (stream) stream->setTimeout(100);
    if (!stream) {
        snprintf(s_dl_status, sizeof(s_dl_status), "error: no stream");
        outf.close();
        http.end();
        if (sc) { delete sc; sc = nullptr; }
        if (is_replace) SD_MMC.rename(backup_path.c_str(), sdpath.c_str());
        SD_MMC.remove(sdpath.c_str());
        s_dl_cancel = false;
        s_dl_active = false;
        lcd_invalidate_layout();
        return;
    }

    // Spin up the SD write task (Core 1, priority 5 > Arduino loop priority 1).
    s_wr_start = xSemaphoreCreateBinary();
    s_wr_done  = xSemaphoreCreateBinary();
    s_wr_file  = &outf;
    s_wr_err   = false;
    s_wr_ptr   = nullptr;
    xTaskCreatePinnedToCore(sd_writer_task, "dl_sd_wr", 4096, nullptr, 5, nullptr, 1);

    uint8_t* rd_buf    = s_buf;               // buffer being filled from TCP
    uint8_t* wr_buf    = s_buf + DL_HALF_SIZE;// buffer free for next SD write (starts empty)
    int32_t  rd_fill   = 0;
    bool     wr_pending = false;
    int32_t  wr_size    = 0;                  // bytes submitted in current write
    int32_t  bytes_recv  = 0;
    int32_t  last_lcd    = -DL_LCD_EVERY;
    bool     write_err   = false;
    bool     cancelled   = false;
    uint32_t speed_t0    = millis();
    int32_t  speed_byt0  = 0;

    while (true) {
        if (s_dl_cancel) { cancelled = true; break; }

        // Poll: did the pending SD write finish? (non-blocking)
        if (wr_pending && xSemaphoreTake(s_wr_done, 0) == pdTRUE) {
            if (s_wr_err) { write_err = true; break; }
            bytes_recv     += wr_size;
            s_dl_bytes_recv = bytes_recv;
            wr_pending = false;
            // wr_buf is now the just-written buffer — free for next swap
        }

        // Drain TCP socket into rd_buf (grab all immediately available bytes)
        {
            int space = DL_HALF_SIZE - rd_fill;
            while (space > 0) {
                int avail = stream->available();
                if (avail <= 0) break;
                int n = stream->readBytes(rd_buf + rd_fill, avail < space ? avail : space);
                if (n <= 0) break;
                rd_fill   += n;
                space     -= n;
            }
        }

        bool stream_done = !http.connected() && stream->available() == 0;
        // Count in-flight bytes (submitted but not yet acknowledged) for have_all check
        int32_t in_flight = wr_pending ? wr_size : 0;
        bool have_all  = known_size &&
                         (bytes_recv + in_flight + rd_fill) >= (int32_t)content_len;

        // Submit rd_buf when the half-buffer fills, stream is done, or all bytes are in hand.
        // Using DL_HALF_SIZE (32 KB) as threshold: blocks every ~80 ms at 400 KB/s instead
        // of every ~10 ms with DL_SUBMIT_THRESH=4096, reducing blocking overhead by ~8×.
        if (rd_fill > 0 && (rd_fill >= DL_HALF_SIZE || stream_done || have_all)) {
            // Wait (blocking) for any in-flight write to complete first
            if (wr_pending) {
                xSemaphoreTake(s_wr_done, portMAX_DELAY);
                if (s_wr_err) { write_err = true; break; }
                bytes_recv     += wr_size;
                s_dl_bytes_recv = bytes_recv;
                wr_pending = false;
            }

            // Hand rd_buf to the write task
            s_wr_ptr  = rd_buf;
            s_wr_len  = (size_t)rd_fill;
            wr_size   = rd_fill;
            xSemaphoreGive(s_wr_start);
            wr_pending = true;

            // Swap: the just-freed wr_buf becomes the new rd_buf
            uint8_t* tmp = rd_buf; rd_buf = wr_buf; wr_buf = tmp;
            rd_fill = 0;

            // Update speed / progress using committed + in-flight bytes
            int32_t visible = bytes_recv + wr_size;
            uint32_t now = millis(), dt = now - speed_t0;
            if (dt >= 1000) {
                s_dl_speed_kbps = (int)((int64_t)(visible - speed_byt0) * 1000 / dt / 1024);
                speed_t0   = now;
                speed_byt0 = visible;
            }
            if (known_size) {
                int pct = (int)((int64_t)visible * 100 / content_len);
                s_dl_progress = pct;
                snprintf(s_dl_status, sizeof(s_dl_status), "downloading (%d%%)", pct);
            } else {
                s_dl_progress = -1;
                snprintf(s_dl_status, sizeof(s_dl_status),
                         "downloading (%d KB)", (int)(visible / 1024));
            }
            if (visible - last_lcd >= (int32_t)DL_LCD_EVERY) {
                last_lcd = visible;
                lcd_show_progress(s_dl_filename,
                                  known_size ? (uint8_t)s_dl_progress : 0,
                                  s_dl_speed_kbps, visible, content_len,
                                  downloader_queue_count());
            }
        }

        // Exit when stream exhausted and all data committed
        if (!wr_pending && rd_fill == 0 && (stream_done || have_all)) break;

        if (rd_fill == 0) {
            vTaskDelay(1);  // nothing to read — yield so lwIP can process ACKs
        }
        // (else: data still accumulating — loop immediately)
    }

    // Wait for the final in-flight SD write
    if (wr_pending) {
        xSemaphoreTake(s_wr_done, portMAX_DELAY);
        if (!write_err) {
            if (s_wr_err) write_err = true;
            else          bytes_recv += wr_size;
        }
        wr_pending = false;
    }
    // Write any residual rd_fill that didn't trigger a submit (shouldn't happen
    // in normal flow, but guard against edge-cases from partial cancelled writes)
    if (!cancelled && !write_err && rd_fill > 0) {
        s_wr_ptr = nullptr;  // don't double-use task; write directly
        size_t w = outf.write(rd_buf, (size_t)rd_fill);
        if (w != (size_t)rd_fill) write_err = true;
        else bytes_recv += rd_fill;
    }

    // Gracefully terminate the SD write task
    s_wr_ptr = nullptr;
    xSemaphoreGive(s_wr_start);   // wake task with nullptr → it exits
    vTaskDelay(20);               // give it time to call vTaskDelete
    vSemaphoreDelete(s_wr_start); s_wr_start = nullptr;
    vSemaphoreDelete(s_wr_done);  s_wr_done  = nullptr;

    outf.close();

    // Limit TLS shutdown wait to 1 s — prevents task watchdog reset on slow servers.
    if (sc) sc->setTimeout(1000);
    http.end();
    if (sc) { delete sc; sc = nullptr; }

    // ── Cancelled ─────────────────────────────────────────────────────────────
    if (cancelled) {
        SD_MMC.remove(sdpath.c_str());
        if (is_replace) SD_MMC.rename(backup_path.c_str(), sdpath.c_str());  // restore original
        s_dl_cancel = false;
        snprintf(s_dl_status, sizeof(s_dl_status), "cancelled");
        s_dl_active = false;
        lcd_invalidate_layout();
        {
            char det[48];
            if (bytes_recv >= 1024) snprintf(det, sizeof(det), "Cancelled @ %d KB", (int)(bytes_recv / 1024));
            else                    strncpy(det, "Cancelled", sizeof(det));
            actlog_add(ACT_CANCEL, s_dl_filename, det);
        }
        Serial.printf("[DL] Cancelled: /%s\n", s_dl_filename);
        return;
    }

    // ── Error: SD write failure ───────────────────────────────────────────────
    if (write_err) {
        SD_MMC.remove(sdpath.c_str());
        if (is_replace) SD_MMC.rename(backup_path.c_str(), sdpath.c_str());
        snprintf(s_dl_status, sizeof(s_dl_status), "error: write failed");
        s_dl_active = false;
        lcd_invalidate_layout();
        {
            char det[48];
            snprintf(det, sizeof(det), "Write error @ %d KB", (int)(bytes_recv / 1024));
            actlog_add(ACT_WARN, s_dl_filename, det);
        }
        return;
    }

    // ── Error: truncated response ─────────────────────────────────────────────
    if (known_size && bytes_recv < (int32_t)content_len) {
        SD_MMC.remove(sdpath.c_str());
        if (is_replace) SD_MMC.rename(backup_path.c_str(), sdpath.c_str());
        snprintf(s_dl_status, sizeof(s_dl_status),
                 "error: incomplete (%d/%d B)", (int)bytes_recv, content_len);
        s_dl_cancel = false;
        s_dl_active = false;
        lcd_invalidate_layout();
        {
            char det[48];
            snprintf(det, sizeof(det), "Incomplete %d/%d KB",
                     (int)(bytes_recv / 1024), (int)(content_len / 1024));
            actlog_add(ACT_WARN, s_dl_filename, det);
        }
        return;
    }

    // ── Success ───────────────────────────────────────────────────────────────
    if (is_replace) SD_MMC.remove(backup_path.c_str());  // discard backup — new file is good
    s_dl_progress = 100;
    snprintf(s_dl_status, sizeof(s_dl_status), "done");
    lcd_show_progress(s_dl_filename, 100, -1, 0, -1, 0);
    delay(800);
    lcd_invalidate_layout();
    s_dl_active = false;

    {
        char dl_detail[48];
        uint32_t elapsed = millis() - dl_start_ms;
        int avg_kbps = (elapsed > 0) ? (int)((int64_t)bytes_recv * 1000 / elapsed / 1024) : -1;
        char size_buf[16], spd_buf[16];
        if (bytes_recv >= 1048576) snprintf(size_buf, sizeof(size_buf), "%.1f MB", bytes_recv / 1048576.0f);
        else                       snprintf(size_buf, sizeof(size_buf), "%d KB",   (int)(bytes_recv / 1024));
        if (avg_kbps >= 1024)      snprintf(spd_buf, sizeof(spd_buf), "%.1f MB/s", avg_kbps / 1024.0f);
        else if (avg_kbps >= 0)    snprintf(spd_buf, sizeof(spd_buf), "%d KB/s", avg_kbps);
        else                       spd_buf[0] = '\0';
        if (spd_buf[0]) snprintf(dl_detail, sizeof(dl_detail), "%s @ %s", size_buf, spd_buf);
        else            snprintf(dl_detail, sizeof(dl_detail), "%s", size_buf);
        actlog_add(ACT_DL, s_dl_filename, dl_detail);
    }
    Serial.printf("[DL] Done: /%s (%d bytes)\n", s_dl_filename, (int)bytes_recv);
}

// Expose quick_filename for web_server response (avoids duplicating logic there)
String downloader_quick_filename(const char* url) {
    return quick_filename(url);
}
