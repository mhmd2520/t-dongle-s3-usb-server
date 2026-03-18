#pragma once
#include <Arduino.h>

// Unicode icon constants (all ≤3 UTF-8 bytes so they fit in the 4-byte icon field)
#define ACT_DL     "\u2B07"   // ⬇ downloaded
#define ACT_CANCEL "\u2715"   // ✕ cancelled
#define ACT_SKIP   "\u23ED"   // ⏭ skipped
#define ACT_DEL    "\u2717"   // ✗ deleted
#define ACT_RENAME "\u270E"   // ✎ renamed
#define ACT_MKDIR  "+"        //   new folder
#define ACT_UPLOAD "\u2B06"   // ⬆ uploaded
#define ACT_WARN   "\u26A0"   // ⚠ error/warning

// Add an entry to the circular log (safe to call from either core — no blocking).
void   actlog_add(const char* icon, const char* name, const char* detail = nullptr);

// Serialize the log to a JSON array string, newest entry first.
String actlog_get_json();
