#pragma once
#include <Arduino.h>

// Queue a URL for download. Called from web handler (Core 0).
// Returns false if a download is already pending or active.
bool        downloader_queue(const char* url);

// True while a download is in progress (Core 1 stream loop).
bool        downloader_is_busy();

// 0-100 if Content-Length known; -1 if chunked/unknown size.
int         downloader_progress();

// Filename being (or last) downloaded, e.g. "file.zip".
const char* downloader_filename();

// "idle" / "downloading (42%)" / "downloading (142 KB)" / "done" / "error: <reason>"
const char* downloader_status();

// Cancel an in-progress download. Safe to call from Core 0 handler context.
// The stream loop will stop, delete the partial file, and set status "cancelled".
void        downloader_cancel();

// Execute pending download. Must be called from main loop (Core 1) only.
void        downloader_run();

// Quick filename parse from URL (no SD collision check) — for API responses.
String      downloader_quick_filename(const char* url);
