#pragma once

// Synchronous file/ZIP download server on port 8080.
//
// WebServer::streamFile() runs in the Arduino loop task (Core 1) via
// dl_server_loop(), where SD_MMC DMA reads work reliably on IDF 5.x.
// Kept in a separate translation unit to avoid HTTP_GET/HTTP_POST name
// collision between WebServer.h (http_parser.h) and ESPAsyncWebServer.h.

void dl_server_begin();
void dl_server_loop();
// Called by web_server when auth credentials change, so port 8080 re-reads NVS.
void dl_auth_cache_invalidate();
