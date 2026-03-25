#pragma once
#include <Arduino.h>

// Add a URL to the download queue. Called from web handler (Core 0).
// Returns false only if the queue is full (DL_QUEUE_SIZE slots).
bool        downloader_queue(const char* url);

// Number of URLs waiting in queue (not counting the actively-running download).
int         downloader_queue_count();

// True while a download is in progress (Core 1 stream loop).
bool        downloader_is_busy();

// 0-100 if Content-Length known; -1 if chunked/unknown size.
int         downloader_progress();

// Current download speed in KB/s.  0 before first sample.
int         downloader_speed();

// Bytes received so far in the active (or last) download.
int32_t     downloader_bytes_recv();

// Total content length in bytes as reported by the server; -1 if unknown.
int         downloader_content_len();

// Filename being (or last) downloaded, e.g. "file.zip".
const char* downloader_filename();

// "idle" / "downloading (42%)" / "downloading (142 KB)" / "done" / "error: <reason>"
const char* downloader_status();

// Cancel an in-progress download. Safe to call from Core 0 handler context.
// The stream loop will stop, delete the partial file, and set status "cancelled".
void        downloader_cancel();

// True when a filename conflict is waiting for user resolution.
bool        downloader_conflict_pending();

// Name of the conflicting file (valid only when downloader_conflict_pending()).
const char* downloader_conflict_name();

// Resolve a pending conflict.
//   "replace"  → overwrite existing file (original backed up as .bak, restored on failure)
//   "skip"     → "Keep Both": download to a numbered filename (file_1.ext, file_2.ext…)
//   "cancel"   → abort download and discard partial file
void        downloader_resolve(const char* action);

// Execute pending download. Must be called from main loop (Core 1) only.
void        downloader_run();

// Quick filename parse from URL (no SD collision check) — for API responses.
String      downloader_quick_filename(const char* url);
