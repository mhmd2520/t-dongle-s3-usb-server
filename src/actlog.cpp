#include "actlog.h"
#include <ArduinoJson.h>

#define LOG_MAX 50

struct LogEntry {
    uint32_t ts_sec;
    char     icon[5];    // ≤3 UTF-8 bytes + null terminator
    char     name[65];
    char     detail[48];
};

static LogEntry s_log[LOG_MAX];
static int      s_log_head  = 0;   // index of the oldest entry
static int      s_log_count = 0;

void actlog_add(const char* icon, const char* name, const char* detail) {
    int idx = (s_log_head + s_log_count) % LOG_MAX;
    s_log[idx].ts_sec = millis() / 1000;

    strncpy(s_log[idx].icon,   icon,   sizeof(s_log[idx].icon)   - 1);
    s_log[idx].icon[sizeof(s_log[idx].icon) - 1] = '\0';

    strncpy(s_log[idx].name,   name,   sizeof(s_log[idx].name)   - 1);
    s_log[idx].name[sizeof(s_log[idx].name) - 1] = '\0';

    if (detail) {
        strncpy(s_log[idx].detail, detail, sizeof(s_log[idx].detail) - 1);
        s_log[idx].detail[sizeof(s_log[idx].detail) - 1] = '\0';
    } else {
        s_log[idx].detail[0] = '\0';
    }

    if (s_log_count < LOG_MAX) {
        s_log_count++;
    } else {
        s_log_head = (s_log_head + 1) % LOG_MAX;   // overwrite oldest
    }
}

String actlog_get_json() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    char ts[12];
    // Emit newest first
    for (int i = s_log_count - 1; i >= 0; i--) {
        int idx = (s_log_head + i) % LOG_MAX;
        uint32_t sec = s_log[idx].ts_sec;
        snprintf(ts, sizeof(ts), "%02lu:%02lu:%02lu",
                 (unsigned long)(sec / 3600),
                 (unsigned long)((sec % 3600) / 60),
                 (unsigned long)(sec % 60));
        JsonObject e = arr.add<JsonObject>();
        e["ts"]     = ts;
        e["icon"]   = s_log[idx].icon;
        e["name"]   = s_log[idx].name;
        e["detail"] = s_log[idx].detail;
    }
    String out;
    serializeJson(doc, out);
    return out;
}
