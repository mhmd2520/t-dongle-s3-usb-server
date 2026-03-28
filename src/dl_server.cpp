#include "dl_server.h"
#include "storage.h"
#include "actlog.h"
#include "lcd.h"
#include "config.h"
#include "downloader.h"
#include <WebServer.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <vector>
#include <lwip/sockets.h>    // lwip_send(), lwip_setsockopt(), TCP_NODELAY, SO_SNDTIMEO

// ── Port 8080 synchronous download server ─────────────────────────────────────
// All handlers run inside dl_server_loop() → Arduino loop() (Core 1).
// SD_MMC DMA is safe on Core 1 on IDF 5.x; unsafe in async_service_task.
//
// Routes:
//   GET /files              — File Manager HTML (served from SD card)
//   GET /dl?path=...        — file/ZIP download (with HTTP Range support)
//   GET /list?path=...      — directory listing (returns JSON)
//   GET /search?q=...       — recursive filename search (returns JSON)
//
// All responses include Access-Control-Allow-Origin: * (cross-origin from port 80).

extern const char* web_server_fileman_html();

static WebServer s_dl_srv(8080);

#define ZIP_TMP_PATH       "/_dl_tmp.zip"
#define FILEMAN_HTML_PATH  "/_fileman.html"
#define DL_BUF_SIZE        16384   // SD read buffer; matches VFS buffer for minimal DMA ops
#define DL_SEND_TIMEOUT_S  300     // abort if peer stops consuming for 5 min (handles background-tab throttling)

static uint8_t s_dl_buf[DL_BUF_SIZE];

// ── Path safety guard ─────────────────────────────────────────────────────────
static bool safe_path(const String& p) {
    return p.startsWith("/") && p.indexOf("..") < 0;
}

// ── System-file filter (mirrors web_server.cpp) ───────────────────────────────
static bool is_system_entry(const String& name) {
    if (name.isEmpty()) return true;
    if (name[0] == '$') return true;
    if (name.equalsIgnoreCase("System Volume Information")) return true;
    if (name.startsWith("._"))  return true;
    if (name.equalsIgnoreCase(".Trashes"))        return true;
    if (name.equalsIgnoreCase(".Spotlight-V100")) return true;
    if (name.equalsIgnoreCase(".fseventsd"))      return true;
    if (name == "_dl_tmp.zip")   return true;
    if (name == "_fileman.html") return true;
    return false;
}

// ── HTTP Basic Auth (mirrors web_server.cpp cache) ────────────────────────────
// Credentials shared with port 80 — same NVS keys. Cache invalidated by
// dl_auth_cache_invalidate() which web_server calls when credentials change.
static String s_auth_user;
static String s_auth_pass;
static bool   s_auth_loaded = false;

void dl_auth_cache_invalidate() {
    s_auth_loaded = false;
}

static bool dl_auth_check() {
    if (!s_auth_loaded) {
        Preferences p;
        p.begin(NVS_NS, true);
        s_auth_user = p.getString(NVS_KEY_AUTH_USER, "");
        s_auth_pass = p.getString(NVS_KEY_AUTH_PASS, "");
        p.end();
        s_auth_loaded = true;
    }
    if (s_auth_user.isEmpty() || s_auth_pass.isEmpty()) return true;
    if (s_dl_srv.authenticate(s_auth_user.c_str(), s_auth_pass.c_str())) return true;
    s_dl_srv.requestAuthentication("USB Drive");
    return false;
}

// ── MIME type lookup ──────────────────────────────────────────────────────────
static String get_mime(const String& path) {
    int dot = path.lastIndexOf('.');
    if (dot < 0) return "application/octet-stream";
    String ext = path.substring(dot + 1);
    ext.toLowerCase();
    if (ext == "pdf")  return "application/pdf";
    if (ext == "zip")  return "application/zip";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png")  return "image/png";
    if (ext == "gif")  return "image/gif";
    if (ext == "mp4")  return "video/mp4";
    if (ext == "mp3")  return "audio/mpeg";
    if (ext == "txt")  return "text/plain";
    if (ext == "htm" || ext == "html") return "text/html";
    if (ext == "json") return "application/json";
    return "application/octet-stream";
}

// ── Write FILEMAN_HTML to SD card ─────────────────────────────────────────────
static void write_fileman_html_to_sd() {
    if (!storage_is_ready()) return;
    const char* html = web_server_fileman_html();
    File f = SD_MMC.open(FILEMAN_HTML_PATH, FILE_WRITE);
    if (!f) { Serial.println("[DL] WARNING: could not write /_fileman.html to SD"); return; }
    size_t len = strlen(html);
    size_t written = 0;
    while (written < len) {
        size_t chunk = min((size_t)4096, len - written);
        size_t w = f.write((const uint8_t*)(html + written), chunk);
        if (w == 0) break;
        written += w;
    }
    f.close();
    if (written < len)
        Serial.printf("[DL] WARNING: /_fileman.html partial write (%u/%u bytes)\n", written, len);
    else
        Serial.printf("[DL] /_fileman.html written to SD (%u bytes)\n", written);
}

// ── SD → TCP streaming ────────────────────────────────────────────────────────
// displayName: when non-null, shows progress on LCD ("PC Download" header).
//              Pass nullptr for internal transfers (e.g. HTML serving) where
//              no LCD feedback is needed.
static void stream_sd_file(File& f, size_t fileSize,
                            const char* displayName = nullptr) {
    if (!s_dl_srv.client().connected()) return;
    int fd = s_dl_srv.client().fd();
    if (fd < 0) { Serial.println("[DL] no fd"); return; }

    int one = 1;
    lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv = { .tv_sec = DL_SEND_TIMEOUT_S, .tv_usec = 0 };
    lwip_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    size_t   sent      = 0;
    uint32_t start_ms  = millis();
    uint32_t last_lcd  = 0;
    bool     show_lcd  = (displayName != nullptr && displayName[0] != '\0');

    if (show_lcd)
        lcd_show_progress(String(displayName), 0, -1, 0, (int)fileSize, 0, "PC Download");

    Serial.printf("[DL] Streaming %u bytes\n", fileSize);

    while (sent < fileSize) {
        size_t want = min(fileSize - sent, (size_t)DL_BUF_SIZE);
        int n = f.read(s_dl_buf, want);
        if (n <= 0) {
            Serial.printf("[DL] SD read error at %u/%u\n", sent, fileSize);
            break;
        }

        const uint8_t* p = s_dl_buf;
        int remain = n;
        while (remain > 0) {
            int w = lwip_send(fd, p, remain, 0);
            if (w <= 0) {
                Serial.printf("[DL] send error errno=%d at %u/%u\n", errno, sent, fileSize);
                goto stream_done;
            }
            p      += w;
            remain -= w;
            sent   += (size_t)w;
        }

        // LCD update every 300 ms — partial refresh (bar + speed rows only)
        if (show_lcd) {
            uint32_t now = millis();
            if (now - last_lcd >= 300) {
                uint8_t pct    = fileSize > 0 ? (uint8_t)(sent * 100 / fileSize) : 0;
                uint32_t el    = now - start_ms;
                int      spd   = el > 200 ? (int)((int64_t)sent * 1000 / el / 1024) : -1;
                lcd_show_progress(String(displayName), pct, spd, (int32_t)sent,
                                  (int)fileSize, 0, "PC Download");
                last_lcd = now;
            }
        }
    }

stream_done:
    if (show_lcd) lcd_invalidate_layout();
    Serial.printf("[DL] Stream: %u/%u bytes%s\n", sent, fileSize,
                  sent >= fileSize ? "" : " (INCOMPLETE)");
}

// ── /files — File Manager HTML ────────────────────────────────────────────────
static void handle_files_html() {
    if (!dl_auth_check()) return;
    s_dl_srv.sendHeader("Access-Control-Allow-Origin", "*");
    if (!storage_is_ready()) {
        s_dl_srv.send(503, "text/plain", "SD not ready"); return;
    }
    File f = SD_MMC.open(FILEMAN_HTML_PATH, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        s_dl_srv.send(503, "text/plain", "UI file missing"); return;
    }
    size_t fileSize = f.size();
    f.setBufferSize(DL_BUF_SIZE);
    s_dl_srv.sendHeader("Cache-Control", "no-store");
    s_dl_srv.setContentLength(fileSize);
    s_dl_srv.send(200, "text/html", "");
    stream_sd_file(f, fileSize);
    f.close();
}

// ── /dl — file download with HTTP Range support ───────────────────────────────
// Range support lets Chrome resume an interrupted download from the byte it
// stalled at rather than restarting from byte 0.
static void handle_dl() {
    if (!dl_auth_check()) return;
    s_dl_srv.sendHeader("Access-Control-Allow-Origin", "*");
    if (!storage_is_ready()) {
        s_dl_srv.send(503, "text/plain", "SD not ready"); return;
    }
    String path = s_dl_srv.hasArg("path") ? s_dl_srv.arg("path") : String();
    if (path.isEmpty() || !safe_path(path)) {
        s_dl_srv.send(400, "text/plain", "invalid path"); return;
    }

    File f = SD_MMC.open(path.c_str(), FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        s_dl_srv.send(404, "text/plain", "Not found"); return;
    }
    size_t fileSize = f.size();
    f.setBufferSize(DL_BUF_SIZE);

    // Determine download filename (ZIP downloads pass explicit ?name=)
    String name = s_dl_srv.hasArg("name")
        ? s_dl_srv.arg("name")
        : path.substring(path.lastIndexOf('/') + 1);

    // Strip characters that would break the Content-Disposition header
    name.replace("\"", "");
    name.replace("\r", "");
    name.replace("\n", "");

    // ── HTTP Range handling ───────────────────────────────────────────────────
    // Chrome sends "Range: bytes=N-" on retry after an interrupted download.
    // Without Range support, we always 200 + full file → Chrome discards
    // already-received bytes and the download loops forever.
    size_t rangeStart = 0;
    size_t rangeEnd   = fileSize > 0 ? fileSize - 1 : 0;
    bool   isRange    = false;

    if (s_dl_srv.hasHeader("Range")) {
        String r = s_dl_srv.header("Range");
        if (r.startsWith("bytes=")) {
            String spec = r.substring(6);
            int dash = spec.indexOf('-');
            if (dash >= 0) {
                // strtoul() handles files >2 GB (String::toInt() is signed 32-bit)
                size_t rs = (size_t)strtoul(spec.substring(0, dash).c_str(), nullptr, 10);
                String es = spec.substring(dash + 1);
                size_t re = es.length() > 0
                    ? (size_t)strtoul(es.c_str(), nullptr, 10)
                    : (fileSize > 0 ? fileSize - 1 : 0);
                if (rs < fileSize && re < fileSize && rs <= re) {
                    rangeStart = rs;
                    rangeEnd   = re;
                    isRange    = true;
                    if (!f.seek(rangeStart)) {
                        f.close();
                        if (path == ZIP_TMP_PATH) {
                            if (!SD_MMC.remove(ZIP_TMP_PATH))
                                Serial.println("[DL] Warning: temp ZIP cleanup failed (range-error path)");
                        }
                        s_dl_srv.send(416, "text/plain", "Range Not Satisfiable");
                        return;
                    }
                }
            }
        }
    }

    size_t sendSize = isRange ? (rangeEnd - rangeStart + 1) : fileSize;

    s_dl_srv.sendHeader("Accept-Ranges", "bytes");
    s_dl_srv.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    if (isRange) {
        String cr = "bytes " + String(rangeStart) + "-" + String(rangeEnd)
                    + "/" + String(fileSize);
        s_dl_srv.sendHeader("Content-Range", cr);
    }
    s_dl_srv.setContentLength(sendSize);
    s_dl_srv.send(isRange ? 206 : 200, get_mime(path).c_str(), "");

    uint32_t t0 = millis();
    stream_sd_file(f, sendSize, isRange ? nullptr : name.c_str());
    uint32_t elapsed_ms = millis() - t0;
    f.close();

    if (!isRange) {
        // Build richer detail: "319 KB @ 212 KB/s" or "2.5 MB @ 1.1 MB/s"
        char detail[48];
        int spd_kbps = (elapsed_ms > 0)
            ? (int)((int64_t)sendSize * 1000 / elapsed_ms / 1024) : -1;
        char size_buf[16], spd_buf[16];
        if (sendSize >= 1048576) snprintf(size_buf, sizeof(size_buf), "%.1f MB", sendSize / 1048576.0f);
        else                     snprintf(size_buf, sizeof(size_buf), "%d KB", (int)(sendSize / 1024));
        if (spd_kbps >= 1024)    snprintf(spd_buf, sizeof(spd_buf), "%.1f MB/s", spd_kbps / 1024.0f);
        else if (spd_kbps >= 0)  snprintf(spd_buf, sizeof(spd_buf), "%d KB/s", spd_kbps);
        else                     spd_buf[0] = '\0';
        if (spd_buf[0])
            snprintf(detail, sizeof(detail), "%s @ %s \u2192 PC", size_buf, spd_buf);
        else
            snprintf(detail, sizeof(detail), "%s \u2192 PC", size_buf);
        actlog_add(ACT_DL, name.c_str(), detail);
    }
    if (path == ZIP_TMP_PATH) {
        if (!SD_MMC.remove(ZIP_TMP_PATH))
            Serial.println("[DL] Warning: temp ZIP cleanup failed");
    }
}

// ── /list — directory listing ─────────────────────────────────────────────────
static void handle_list() {
    if (!dl_auth_check()) return;
    s_dl_srv.sendHeader("Access-Control-Allow-Origin", "*");
    if (!storage_is_ready()) {
        s_dl_srv.send(503, "application/json", "{\"error\":\"SD not ready\"}"); return;
    }
    String path = s_dl_srv.hasArg("path") ? s_dl_srv.arg("path") : String("/");
    if (path.isEmpty()) path = "/";
    if (!safe_path(path)) {
        s_dl_srv.send(400, "application/json", "{\"error\":\"invalid path\"}"); return;
    }

    String vfsRoot = String("/sdcard") + (path == "/" ? "" : path);
    DIR* d = opendir(vfsRoot.c_str());
    if (!d) {
        s_dl_srv.send(404, "application/json", "{\"error\":\"not a directory\"}"); return;
    }

    struct LE { String name; bool isDir; uint32_t size; };
    std::vector<LE> entries;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        String name = ent->d_name;
        if (name.isEmpty() || name == "." || name == ".." || is_system_entry(name)) continue;
        bool isDir = (ent->d_type == DT_DIR);
        uint32_t size = 0;
        if (!isDir) {
            struct stat st;
            String vfsPath = vfsRoot + "/" + name;
            if (stat(vfsPath.c_str(), &st) == 0) size = (uint32_t)st.st_size;
        }
        entries.push_back({name, isDir, size});
    }
    closedir(d);

    std::sort(entries.begin(), entries.end(), [](const LE& a, const LE& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    JsonDocument doc;
    doc["path"]     = path;
    doc["free_gb"]  = storage_free_gb();
    doc["total_gb"] = storage_total_gb();
    JsonArray arr = doc["entries"].to<JsonArray>();
    for (const auto& e : entries) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = e.name;
        obj["dir"]  = e.isDir;
        obj["size"] = e.size;
    }
    String json;
    serializeJson(doc, json);
    s_dl_srv.send(200, "application/json", json);
}

// ── /search — recursive filename search ──────────────────────────────────────
static void search_walk(const String& sdp, const String& q, JsonArray arr, int& cnt, int depth = 0) {
    if (cnt >= 200 || depth > 20) return;
    File dir = SD_MMC.open(sdp);
    if (!dir || !dir.isDirectory()) { dir.close(); return; }
    File f = dir.openNextFile();
    while (f && cnt < 200) {
        String fn = f.name();
        int sl = fn.lastIndexOf('/');
        String base = sl >= 0 ? fn.substring(sl + 1) : fn;
        if (base.length() > 0 && !is_system_entry(base)) {
            String bl = base; bl.toLowerCase();
            if (bl.indexOf(q) >= 0) {
                JsonObject obj = arr.add<JsonObject>();
                obj["name"]   = base;
                obj["path"]   = fn;
                obj["parent"] = sl > 0 ? fn.substring(0, sl) : String("/");
                obj["dir"]    = f.isDirectory();
                if (!f.isDirectory()) {
                    struct stat st;
                    String vfsPath = String("/sdcard") + fn;
                    uint32_t sz = 0;
                    if (stat(vfsPath.c_str(), &st) == 0) sz = (uint32_t)st.st_size;
                    obj["size"] = sz;
                }
                cnt++;
            }
            if (f.isDirectory()) search_walk(fn, q, arr, cnt, depth + 1);
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
}

static void handle_search() {
    if (!dl_auth_check()) return;
    s_dl_srv.sendHeader("Access-Control-Allow-Origin", "*");
    if (!storage_is_ready()) {
        s_dl_srv.send(503, "application/json", "{\"error\":\"SD not ready\"}"); return;
    }
    String q = s_dl_srv.hasArg("q") ? s_dl_srv.arg("q") : String();
    q.toLowerCase();
    if (q.isEmpty()) {
        s_dl_srv.send(400, "application/json", "{\"error\":\"q required\"}"); return;
    }

    JsonDocument doc;
    JsonArray arr = doc["entries"].to<JsonArray>();
    doc["free_gb"]  = storage_free_gb();
    doc["total_gb"] = storage_total_gb();
    int cnt = 0;
    search_walk("/", q, arr, cnt);
    String json;
    serializeJson(doc, json);
    s_dl_srv.send(200, "application/json", json);
}

// ── /manifest-sync — read /_manifest.json from SD, queue new URLs ─────────────
// Runs on Core 1 (dl_server_loop), so SD reads and downloader_queue() are safe.
// Manifest format: {"files":[{"url":"https://..."},{"url":"...","path":"/dir/name.ext"}]}
// Files already present on SD are skipped; only new files are queued for download.
#define MANIFEST_PATH     "/_manifest.json"
#define MANIFEST_BUF_SIZE 4096

static void handle_manifest_sync() {
    if (!dl_auth_check()) return;
    s_dl_srv.sendHeader("Access-Control-Allow-Origin", "*");
    if (!storage_is_ready()) {
        s_dl_srv.send(503, "application/json", "{\"error\":\"SD not ready\"}"); return;
    }

    File mf = SD_MMC.open(MANIFEST_PATH, FILE_READ);
    if (!mf || mf.isDirectory()) {
        if (mf) mf.close();
        s_dl_srv.send(404, "application/json",
                      "{\"error\":\"/_manifest.json not found on SD\"}");
        return;
    }

    // Read manifest (cap at MANIFEST_BUF_SIZE to protect heap).
    static char s_manifest_buf[MANIFEST_BUF_SIZE + 1];
    size_t n = mf.read((uint8_t*)s_manifest_buf, MANIFEST_BUF_SIZE);
    mf.close();
    if (n == 0) {
        s_dl_srv.send(400, "application/json", "{\"error\":\"empty manifest\"}"); return;
    }
    s_manifest_buf[n] = '\0';

    JsonDocument doc;
    DeserializationError jerr = deserializeJson(doc, s_manifest_buf);
    if (jerr) {
        s_dl_srv.send(400, "application/json", "{\"error\":\"invalid JSON\"}"); return;
    }
    JsonArray files = doc["files"].as<JsonArray>();
    if (files.isNull()) {
        s_dl_srv.send(400, "application/json",
                      "{\"error\":\"missing 'files' array\"}"); return;
    }

    int queued = 0, skipped = 0, q_full = 0;
    for (JsonObject entry : files) {
        const char* url  = entry["url"];
        const char* path = entry["path"];   // optional explicit target path

        if (!url || url[0] == '\0') { skipped++; continue; }

        // Determine target path for the existence check.
        String target;
        if (path && path[0] != '\0') {
            target = String(path);
        } else {
            target = "/" + downloader_quick_filename(url);
        }

        // Skip files already present on SD (idempotent sync).
        if (SD_MMC.exists(target.c_str())) { skipped++; continue; }

        if (downloader_queue(url)) {
            queued++;
        } else {
            q_full++;   // DL_QUEUE_SIZE exceeded
        }
    }

    char resp[96];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"queued\":%d,\"skipped\":%d,\"queue_full\":%d}",
             queued, skipped, q_full);
    s_dl_srv.send(200, "application/json", resp);
}

// ── Public API ────────────────────────────────────────────────────────────────

void dl_server_begin() {
    // Remove stale temp ZIP from any previous session (aborted/interrupted download).
    SD_MMC.remove(ZIP_TMP_PATH);
    write_fileman_html_to_sd();
    const char* collectHdrs[] = { "Range" };
    s_dl_srv.collectHeaders(collectHdrs, 1);
    s_dl_srv.on("/files",         HTTP_GET,  handle_files_html);
    s_dl_srv.on("/dl",            HTTP_GET,  handle_dl);
    s_dl_srv.on("/list",          HTTP_GET,  handle_list);
    s_dl_srv.on("/search",        HTTP_GET,  handle_search);
    s_dl_srv.on("/manifest-sync", HTTP_POST, handle_manifest_sync);
    s_dl_srv.begin();
    Serial.println("[HTTP] Download server started on port 8080");
}

void dl_server_loop() {
    s_dl_srv.handleClient();
}
