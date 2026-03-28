#include "web_server.h"
#include "config.h"
#include "wifi_manager.h"
#include "storage.h"
#include "themes.h"
#include "lcd.h"
#include "downloader.h"
#include "actlog.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <vector>
#include <memory>
#include <algorithm>
#include <ESPAsyncWebServer.h>
#include "dl_server.h"

// ── Server instances ───────────────────────────────────────────────────────────
// Port 80: ESPAsyncWebServer — all API routes, UI, upload, URL-downloader.
// Port 8080: dl_server (dl_server.cpp) — synchronous WebServer, file/ZIP download.
//   WebServer.h and ESPAsyncWebServer.h both expose HTTP_GET in the global namespace
//   (via http_parser.h), causing ambiguity. Separating them into different
//   translation units avoids the collision entirely.

static AsyncWebServer server(80);

// Non-zero = restart at this millis() timestamp (after response is sent).
static uint32_t g_restart_at = 0;

// ── HTTP Basic Auth ────────────────────────────────────────────────────────────
// Credentials are stored in NVS under NVS_NS/NVS_KEY_AUTH_USER+PASS.
// Empty username or password = no auth (open access, backwards compatible).
// Cache is invalidated by auth_cache_invalidate() when credentials change.
static String s_auth_user;
static String s_auth_pass;
static bool   s_auth_loaded = false;

static void auth_load() {
    if (s_auth_loaded) return;
    Preferences p;
    p.begin(NVS_NS, true);
    s_auth_user = p.getString(NVS_KEY_AUTH_USER, "");
    s_auth_pass = p.getString(NVS_KEY_AUTH_PASS, "");
    p.end();
    s_auth_loaded = true;
}

static void auth_cache_invalidate() {
    s_auth_loaded = false;
    dl_auth_cache_invalidate();  // keep port 8080 in sync
}

// Returns true if authorized (or no credentials set). Sends 401 and returns
// false if the request lacks valid credentials. Insert at the top of every
// handler that should be protected. handle_not_found is exempt (captive portal).
static bool auth_check(AsyncWebServerRequest* req) {
    auth_load();
    if (s_auth_user.isEmpty() || s_auth_pass.isEmpty()) return true;
    if (req->authenticate(s_auth_user.c_str(), s_auth_pass.c_str())) return true;
    req->requestAuthentication("USB Drive");
    return false;
}

// ── Busy-flag: prevents concurrent heavy SD operations ────────────────────────
// AsyncWebServer can serve multiple requests concurrently on dual-core ESP32-S3.
// Heavy SD operations (download / ZIP / upload) set this flag; a second request
// for any heavy operation returns 503 immediately to avoid SD state corruption.
static portMUX_TYPE   s_busy_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool  s_busy     = false;
static const char*    s_busy_op  = "";

static bool busy_try(const char* op) {
    bool ok = false;
    taskENTER_CRITICAL(&s_busy_mux);
    if (!s_busy) { s_busy = true; s_busy_op = op; ok = true; }
    taskEXIT_CRITICAL(&s_busy_mux);
    return ok;
}
static void busy_clear() {
    taskENTER_CRITICAL(&s_busy_mux);
    s_busy = false; s_busy_op = "";
    taskEXIT_CRITICAL(&s_busy_mux);
}

// ── Deferred ZIP build state ───────────────────────────────────────────────────
// ESPAsyncWebServer sends HTTP 501 if a handler returns without calling req->send().
// Solution: handle_download_zip() responds immediately with {"status":"building"};
// run_deferred_zip() does SD work in web_server_loop() (Core 1) and sets state;
// JS polls /api/zip-status until done, then navigates to port 8080 for the file.
enum ZipBuildState { ZIP_STATE_IDLE = 0, ZIP_STATE_BUILDING, ZIP_STATE_DONE, ZIP_STATE_ERROR };
static volatile ZipBuildState g_zip_state     = ZIP_STATE_IDLE;
static volatile bool          g_zip_requested = false;
// Fixed-size buffer — avoids cross-core heap races (String heap alloc is not
// Core-safe). Written on Core 0 before g_zip_requested is set; Core 1 reads
// only after observing g_zip_requested — the volatile write acts as barrier.
static char                   g_zip_req_path[256];
// When non-empty, zip_run builds a ZIP of these specific comma-separated SD paths
// instead of walking a directory. Written on Core 0 before g_zip_requested is set.
static char                   g_zip_sel_paths[2048];
// Fixed-size buffer avoids cross-core heap races (String uses heap).
// Written on Core 1 before g_zip_state is set to DONE; Core 0 reads only
// after observing DONE — g_zip_state acts as the release/acquire barrier.
static char                   g_zip_done_json[256];

// ── Upload LCD state ───────────────────────────────────────────────────────────
// handle_upload_body() runs on Core 0 (async_service_task); writing LCD from
// Core 0 risks SPI contention with Core 1.  Instead, Core 0 updates these
// volatile vars and web_server_loop() (Core 1) drives the LCD.
static volatile bool     g_upload_lcd_active = false;
static char              g_upload_lcd_name[65] = "";   // protected by g_upload_lcd_active handshake
static volatile uint32_t g_upload_lcd_bytes  = 0;
static volatile uint32_t g_upload_lcd_total  = 0;
static uint32_t          g_upload_lcd_start  = 0;      // written/read only on Core 1 via web_server_loop


// ── CRC-32/ISO-HDLC (ZIP standard) ────────────────────────────────────────────
// 256-entry byte-at-a-time table — 4× faster than the 16-entry nibble table
// (1 lookup per byte vs 2).  Table is generated once on first call and cached
// in SRAM (~1 KB).  Only called from Core 1 (run_deferred_zip), so the
// one-time init is race-free.
// Polynomial: 0xEDB88320 (reflected IEEE 802.3 / PKZIP CRC-32).
// Test vector: crc32_feed(0xFFFFFFFF,"123456789",9)^0xFFFFFFFF == 0xCBF43926
static uint32_t crc32_feed(uint32_t state, const uint8_t* buf, size_t len) {
    static uint32_t T[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            T[i] = c;
        }
        built = true;
    }
    for (size_t i = 0; i < len; i++) {
        state = T[(state ^ buf[i]) & 0xFF] ^ (state >> 8);
    }
    return state;
}

// ── Cached WiFi scan — manual only in all modes ───────────────────────────────
// No automatic background scanning in either AP or STA mode.
// The ↻ Refresh button sends /api/scan?start=1 which triggers one scan.
// handle_scan returns the cached result immediately; the browser JS polls
// every 1 s via refreshNets() until fresh results arrive (~2-3 s scan time).

static String   g_scan_cache;

static String get_networks_html() {
    int n = WiFi.scanComplete();  // -2=running, -1=idle, >=0=results
    if (n >= 0) {
        // Sort by RSSI descending (strongest signal first, e.g. -45 > -80 dBm).
        std::vector<int> idx;
        idx.reserve(n);
        for (int i = 0; i < n; i++) idx.push_back(i);
        std::sort(idx.begin(), idx.end(), [](int a, int b) {
            return WiFi.RSSI(a) > WiFi.RSSI(b);
        });
        g_scan_cache = "";
        for (int i : idx) {
            String s = WiFi.SSID(i);
            // HTML-escape SSID to prevent XSS via malicious AP names.
            String esc = s;
            esc.replace("&", "&amp;");
            esc.replace("\"", "&quot;");
            esc.replace("<", "&lt;");
            g_scan_cache += "<option value=\"" + esc + "\">"
                          + esc + " (" + WiFi.RSSI(i) + " dBm)</option>";
        }
        if (g_scan_cache.isEmpty())
            g_scan_cache = "<option value=''>No networks found</option>";
        WiFi.scanDelete();
    }
    return g_scan_cache.isEmpty()
        ? String("<option value=''>Scanning\u2026</option>")
        : g_scan_cache;
}

// ── Helper ─────────────────────────────────────────────────────────────────────

static String qparam(AsyncWebServerRequest* req, const char* key) {
    // Check POST body params first (application/x-www-form-urlencoded),
    // then fall back to query string. Without post=true, getParam() only
    // searches query parameters and all POST handlers silently return 400.
    auto* p = req->getParam(key, true);   // body param
    if (!p) p = req->getParam(key);       // query param
    return p ? p->value() : String();
}

static void send_json(AsyncWebServerRequest* req, int code, const String& json) {
    req->send(code, "application/json", json);
}

static void send_busy(AsyncWebServerRequest* req) {
    String j = "{\"error\":\"busy\",\"op\":\"";
    j += s_busy_op; j += "\"}";
    req->send(503, "application/json", j);
}

// ── File Manager HTML ─────────────────────────────────────────────────────────
// Upload: POST /upload?path=/folder/file.txt  Content-Type: application/octet-stream
// (raw binary body, avoids multipart parsing complexity)

static const char FILEMAN_HTML[] =
R"html(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>File Manager</title><style>
*{box-sizing:border-box;margin:0;padding:0}body{font-family:system-ui,sans-serif;background:#0d0d0d;color:#eee;max-width:600px;margin:0 auto;padding:12px}
h1{color:#0cf;font-size:1.1em;padding:8px 0 4px;display:flex;justify-content:space-between;align-items:center}h1 a{color:#0cf;font-size:.8em;font-weight:400;text-decoration:none}
.bc{font-size:.76em;color:#555;margin-bottom:8px;word-break:break-all}.bc a{color:#0cf;text-decoration:none}
.card{background:#141414;border:1px solid #1e1e1e;border-radius:8px;padding:12px;margin-bottom:12px}
.row{display:flex;align-items:center;padding:5px 0;border-bottom:1px solid #1a1a1a;font-size:.84em;gap:5px}.row:last-child{border:0}
.nm{flex:1;overflow:hidden;min-width:0}.nm a{color:#eee;text-decoration:none}.nm a:hover{color:#0cf}.nm a.img{color:#8df;cursor:pointer}
.nm>.fn{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.np{font-size:.7em;color:#555;display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.sz{color:#555;font-size:.75em;min-width:50px;text-align:right;flex-shrink:0}
.act{padding:2px 4px;cursor:pointer;background:none;border:none;font-size:.82em}.del{color:#f44}.dld{color:#0cf;cursor:pointer;background:none;border:none;font-size:.82em;padding:2px 4px}.ren{color:#fa0;cursor:pointer;background:none;border:none;font-size:.82em;padding:2px 4px}
.toast{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);background:#1a1a2e;border:1px solid #0cf;border-radius:8px;color:#0cf;padding:8px 14px;font-size:.82em;z-index:999;display:none;max-width:92vw;text-align:center;box-shadow:0 4px 16px #000a;align-items:center;gap:10px}
.tcnc{background:#400;color:#f88;border:none;border-radius:4px;padding:2px 9px;cursor:pointer;font-size:.85em;font-weight:700;flex-shrink:0}
.bar{display:flex;gap:5px;margin-bottom:8px;flex-wrap:wrap;align-items:center}
.btn{padding:5px 9px;border:none;border-radius:5px;font-size:.78em;font-weight:600;cursor:pointer}
.c{background:#0cf;color:#000}.g{background:#1a1a1a;color:#aaa}.rd{background:#922;color:#fff}
.srchw{display:flex;flex:1;min-width:80px;gap:3px;align-items:center}
.srch{flex:1;min-width:0;padding:5px 8px;background:#1c1c1c;border:1px solid #2a2a2a;border-radius:5px;color:#eee;font-size:.78em}
.stor{font-size:.75em;color:#aaa;white-space:nowrap;font-weight:500}
.uf{background:#1c1c1c;border:1px solid #2a2a2a;border-radius:5px;padding:7px;width:100%;color:#eee;font-size:.8em;margin-bottom:6px}
.msg{padding:5px 10px;border-radius:4px;font-size:.78em;display:none}.msg.ok{background:#0a2a0a;color:#0f0;display:block}.msg.ng{background:#2a0a0a;color:#f44;display:block}
progress{width:100%;height:6px;margin-top:5px;display:none}.spd{font-size:.75em;color:#0cf;margin-top:3px;min-height:1.1em}
.selbar{display:none;gap:6px;align-items:center;margin-bottom:8px;font-size:.8em;color:#fa0;flex-wrap:wrap}
.chk{accent-color:#0cf;cursor:pointer;flex-shrink:0}
.srhdr{font-size:.78em;color:#fa0;padding:4px 0 6px;display:none}
.dlbar{display:flex;gap:5px;margin-bottom:8px;align-items:center}
.dlin{flex:1;min-width:0;padding:5px 8px;background:#1c1c1c;border:1px solid #2a2a2a;border-radius:5px;color:#eee;font-size:.78em}
.dlin::placeholder{color:#444}
.lgsec{margin-top:12px;border:1px solid #222;border-radius:6px;overflow:hidden}.lghdr{background:#111;padding:7px 10px;cursor:pointer;display:flex;justify-content:space-between;font-size:.8em;color:#777;user-select:none}.lge{display:flex;gap:8px;padding:3px 0;border-bottom:1px solid #0a0a0a;font-size:.74em;align-items:baseline}.lgts{color:#555;min-width:58px;flex-shrink:0}.lgop{min-width:18px;text-align:center;flex-shrink:0}.lgfn{color:#bbb;flex:1;word-break:break-all}.lgdt{color:#666;flex-shrink:0}
</style></head><body>
<div class="toast" id="toast"><span id="toastTxt"></span><button class="tcnc" id="cancelBtn" onclick="globalCancel()" style="display:none">&#x2715; Cancel</button></div>
<div id="cfModal" style="display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.8);z-index:1000;justify-content:center;align-items:center"><div style="background:#1a1a1a;border:1px solid #fa0;border-radius:8px;padding:20px;max-width:300px;width:90%;text-align:center"><div style="color:#fa0;margin-bottom:8px;font-size:.85em">&#9888; File already exists</div><div id="cfName" style="color:#ccc;font-size:.8em;word-break:break-all;margin-bottom:14px"></div><div style="display:flex;gap:8px;justify-content:center"><button class="btn" style="background:#060" onclick="dlResolve('replace')">Replace</button><button class="btn" style="background:#440" onclick="dlResolve('skip')">Keep Both</button><button class="btn" style="background:#400" onclick="dlResolve('cancel')">Cancel</button></div></div></div>
<h1>&#128190; File Manager <a href="/" id="cfglink" onclick="window.location.href=H+'/';return false;">&#8592; Config</a></h1>
<div class="bc" id="bread"></div>
<div class="card">
<div class="bar">
  <button class="btn c" id="bk" onclick="goBack()" disabled>&#8592;</button>
  <button class="btn c" id="fw" onclick="goFwd()" disabled>&#8594;</button>
  <button class="btn c" onclick="togUp()">&#8679; Upload</button>
  <button class="btn c" onclick="mkD()">&#10133; New Folder</button>
  <button class="btn g" id="sortBtn" onclick="togSort()">&#8645; Name</button>
  <button class="btn g" onclick="togSelAll()">&#9745; All</button>
  <div class="srchw">
    <input class="srch" id="srch" placeholder="Search&#8230;" oninput="onSrch()">
    <button class="btn g" id="srchX" onclick="clrSrch()" style="display:none;padding:5px 7px" title="Clear search">&#10005;</button>
  </div>
  <span class="stor" id="stor"></span>
</div>
<div class="dlbar">
  <input class="dlin" type="url" id="dlurl" placeholder="Paste URL to download to SD&#8230;" autocomplete="off">
  <button class="btn c" onclick="dlUrl()">&#8659; URL</button>
  <button class="btn" id="dlcancel" style="display:none;background:#400;color:#f88" onclick="dlCancel()">&#x2715; Cancel</button>
</div>
<div class="selbar" id="selbar">
  <input type="checkbox" class="chk" id="chkAll" onchange="selAll(this.checked)">
  <span id="selcnt">0 selected</span>
  <button class="btn c" onclick="dlSel()">&#8659; Download</button>
  <button class="btn rd" onclick="delSel()">&#128465; Delete</button>
</div>
<div class="srhdr" id="srhdr"></div>
<div id="upbox" style="display:none;margin-bottom:10px">
  <div style="display:flex;gap:5px;margin-bottom:6px">
    <button class="btn" id="upMF" style="background:#0cf;color:#000;font-size:.75em;padding:3px 9px" onclick="setUpMode(0)">&#128196; Files</button>
    <button class="btn" id="upMD" style="background:#1a1a1a;color:#aaa;font-size:.75em;padding:3px 9px" onclick="setUpMode(1)">&#128193; Folder</button>
  </div>
  <input class="uf" type="file" id="fiUpF" multiple>
  <input class="uf" type="file" id="fiUpD" webkitdirectory style="display:none">
  <button class="btn c" onclick="doUp()">Start Upload</button>
  <progress id="pgUp"></progress><div class="spd" id="spdUp"></div>
  <div class="msg" id="umUp"></div>
</div>
<div id="ls"></div>
</div>
<div class="lgsec"><div class="lghdr" onclick="toggleLog()">&#128221; Activity Log <span id="lgtog">&#9660;</span></div><div id="lgbody" style="display:none;padding:6px 4px;max-height:180px;overflow-y:auto"></div></div>
<script>var H='http://'+location.hostname;
var cp='/',hist=['/'],hIdx=0,g_ent=[],g_sort=0,g_sm=false,g_ent_bk=[],g_st=null,g_busy=0;
var g_toastTimer=null;
function showToast(msg,persist){var t=document.getElementById('toast');document.getElementById('toastTxt').textContent=msg;t.style.display=msg?'flex':'none';if(g_toastTimer){clearTimeout(g_toastTimer);g_toastTimer=null;}if(msg&&!persist&&!g_dlPoll){g_toastTimer=setTimeout(function(){showToast('');},3000);}}
function setBusy(on,msg){g_busy=Math.max(0,g_busy+(on?1:-1));if(g_busy>0){showToast(msg||'\u23f3 Working\u2026',true);}else{showToast('');}}
function navLock(on){document.getElementById('bk').disabled=on||(hIdx===0);document.getElementById('fw').disabled=on||(hIdx>=hist.length-1);}
var SORTS=['&#8645; Name','&#8645; Name&#8595;','&#8645; Size&#8595;'];
function fs(n){return n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':n<1073741824?(n/1048576).toFixed(1)+' MB':(n/1073741824).toFixed(2)+' GB';}
function ft(s){if(s<1)return '<1s';return s<60?Math.ceil(s)+'s':Math.floor(s/60)+'m '+Math.ceil(s%60)+'s';}
function fspd(k){return k<=0?'':k>=1024?(k/1024).toFixed(1)+' MB/s':k+' KB/s';}
function isImg(n){return /\.(jpe?g|png|gif|webp|bmp|svg)$/i.test(n);}
function updNav(){if(g_busy>0)return;document.getElementById('bk').disabled=hIdx===0;document.getElementById('fw').disabled=hIdx>=hist.length-1;}
function setBread(p){var b='<a href="#" onclick="goTo(\'/\')">root</a>';var pts=p.split('/').filter(Boolean),a='';pts.forEach(function(x){a+='/'+x;var pa=a;b+=' / <a href="#" onclick="goTo(\''+je(pa)+'\')">'+ha(x)+'</a>';});document.getElementById('bread').innerHTML=b;}
function goTo(p){if(g_sm)clrSrch();hist=hist.slice(0,hIdx+1);if(hist[hIdx]!==p){hist.push(p);hIdx=hist.length-1;}loadDir(p);updNav();}
function goBack(){if(hIdx>0){hIdx--;loadDir(hist[hIdx]);updNav();}}
function goFwd(){if(hIdx<hist.length-1){hIdx++;loadDir(hist[hIdx]);updNav();}}
function togSort(){g_sort=(g_sort+1)%3;document.getElementById('sortBtn').innerHTML=SORTS[g_sort];renderDir();}
function onSrch(){var q=document.getElementById('srch').value;document.getElementById('srchX').style.display=q?'':'none';clearTimeout(g_st);if(q.trim()){g_st=setTimeout(searchAll,500);}else{if(g_sm){g_sm=false;g_ent=g_ent_bk;document.getElementById('srhdr').style.display='none';}renderDir();}}
function clrSrch(){clearTimeout(g_st);document.getElementById('srch').value='';document.getElementById('srchX').style.display='none';if(g_sm){g_sm=false;g_ent=g_ent_bk;document.getElementById('srhdr').style.display='none';}renderDir();}
async function searchAll(){var q=document.getElementById('srch').value.trim();if(!q)return;document.getElementById('ls').innerHTML='<div style="color:#555;padding:8px;font-size:.8em">Searching&#8230;</div>';
try{var r=await fetch('http://'+location.hostname+':8080/search?q='+encodeURIComponent(q));var d=await r.json();
if(!g_sm){g_ent_bk=g_ent.slice();g_sm=true;}g_ent=d.entries||[];
var hdr=document.getElementById('srhdr');hdr.textContent='Results for "'+q+'" \u2014 '+g_ent.length+(g_ent.length===200?' (capped)':'')+' found';hdr.style.display='block';
renderDir();}catch(ex){document.getElementById('ls').innerHTML='<div style="color:#f44;padding:8px;font-size:.8em">Search error</div>';}}
function sorted(){var q=document.getElementById('srch').value.trim().toLowerCase();
var arr=g_sm?g_ent.slice():g_ent.filter(function(e){return!q||e.name.toLowerCase().indexOf(q)>=0;});
if(!g_sm){if(g_sort===0)arr.sort(function(a,b){return a.dir===b.dir?a.name.localeCompare(b.name):a.dir?-1:1;});
else if(g_sort===1)arr.sort(function(a,b){return a.dir===b.dir?b.name.localeCompare(a.name):a.dir?-1:1;});
else arr.sort(function(a,b){return a.dir===b.dir?b.size-a.size:a.dir?-1:1;});}return arr;}
function renderDir(){var arr=sorted();var h='';
if(!g_sm&&cp!=='/'&&cp!=''){var pp=cp.lastIndexOf('/')>0?cp.substring(0,cp.lastIndexOf('/')):'/';h+='<div class="row"><span>&#128193;</span><span class="nm"><a href="#" class="fn" onclick="goTo(\''+je(pp)+'\')">  ..</a></span></div>';}
arr.forEach(function(e){var fp=g_sm?(e.path||((cp==='/'?'':cp)+'/'+e.name)):(cp==='/'?'':cp)+'/'+e.name;var fpe=encodeURIComponent2(fp);
if(e.dir){
h+='<div class="row"><input type="checkbox" class="chk item-chk" data-path="'+ha(fp)+'" data-dir="1" onchange="updSel()"><span>&#128193;</span>';
h+='<span class="nm"><a href="#" class="fn" onclick="goTo(\''+je(fp)+'\')">'+ha(e.name)+'</a>';
if(g_sm&&e.parent){h+='<span class="np">'+ha(e.parent)+'</span>';}
h+='</span>';
h+='<button class="ren" onclick="rnm(\''+je(fp)+'\',\''+je(e.name)+'\')" title="Rename">&#9998;</button>';
h+='<button class="dld" onclick="dlBlob(H+\'/download-zip?path='+fpe+'\',\''+je(e.name)+'.zip\')" title="ZIP">&#8659;zip</button>';
h+='<button class="act del" onclick="rm(\''+je(fp)+'\')">&#128465;</button></div>';}
else{
h+='<div class="row"><input type="checkbox" class="chk item-chk" data-path="'+ha(fp)+'" data-dir="0" onchange="updSel()"><span>&#128196;</span>';
h+='<span class="nm"><a href="#" class="fn'+(isImg(e.name)?' img':'')+'" onclick="dlFetch(\'http://\'+location.hostname+\':8080/dl?path='+fpe+'\',\''+je(e.name)+'\')">'+ha(e.name)+'</a>';
if(g_sm&&e.parent){h+='<span class="np">'+ha(e.parent)+'</span>';}
h+='</span>';
h+='<span class="sz">'+fs(e.size||0)+'</span>';
h+='<button class="ren" onclick="rnm(\''+je(fp)+'\',\''+je(e.name)+'\')" title="Rename">&#9998;</button>';
h+='<button class="dld" onclick="dlFetch(\'http://\'+location.hostname+\':8080/dl?path='+fpe+'\',\''+je(e.name)+'\')" title="Download">&#8659;</button>';
h+='<button class="act del" onclick="rm(\''+je(fp)+'\')">&#128465;</button></div>';}
});document.getElementById('ls').innerHTML=h||'<div style="color:#555;padding:8px;font-size:.8em">'+(g_sm?'No results':'Empty folder')+'</div>';updSelBar();}
async function loadDir(p){cp=p;setBread(p);document.getElementById('ls').innerHTML='<div style="color:#aaa;padding:8px;font-size:.8em">Loading&#8230;</div>';
try{var r=await fetch('http://'+location.hostname+':8080/list?path='+encodeURIComponent(p));var sd=await r.json();
if(sd.error){document.getElementById('ls').innerHTML='<div style="color:#f44;padding:8px;font-size:.8em">'+ha(sd.error)+'</div>';return;}
if(sd.free_gb!=null)document.getElementById('stor').textContent='Free: '+sd.free_gb.toFixed(1)+'/'+sd.total_gb.toFixed(0)+' GB';g_ent=sd.entries||[];renderDir();}
catch(ex){document.getElementById('ls').innerHTML='<div style="color:#f44;padding:8px;font-size:.8em">Load error</div>';}}
function encodeURIComponent2(p){return encodeURIComponent(p).replace(/'/g,'%27');}
function je(s){return s.replace(/\\/g,'\\\\').replace(/'/g,"\\'");}
function ha(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
async function dlFetch(url,name){
  var ctrl=new AbortController();
  g_cancelFn=function(){ctrl.abort();};
  setBusy(true,'\u2b07 '+name);navLock(true);dlCancelBtn(true);
  var t0=Date.now(),lastUI=0;
  try{
    var r=await fetch(url,{signal:ctrl.signal});
    if(!r.ok){alert('Download error: HTTP '+r.status);return;}
    var total=parseInt(r.headers.get('Content-Length')||'0');
    var reader=r.body.getReader();
    var chunks=[];var got=0;
    while(true){
      var x=await reader.read();
      if(x.done)break;
      chunks.push(x.value);got+=x.value.length;
      var now=Date.now();
      if(now-lastUI>=300){
        lastUI=now;
        var el=now-t0||1,sp=got/el*1000;
        var parts=['\u2b07 '+name];
        if(total>0){parts.push(Math.round(got*100/total)+'%');parts.push(fs(got)+'/'+fs(total));}
        else{parts.push(fs(got));}
        if(sp>500)parts.push(fs(sp)+'/s');
        if(total>0&&sp>500){var eta=(total-got)/sp;if(eta>1)parts.push('ETA '+ft(eta));}
        showToast(parts.join(' \u00b7 '),true);
      }
    }
    var blob=new Blob(chunks,{type:'application/octet-stream'});
    var bu=URL.createObjectURL(blob);
    var a=document.createElement('a');a.href=bu;a.download=name;
    document.body.appendChild(a);a.click();document.body.removeChild(a);
    setTimeout(function(){URL.revokeObjectURL(bu);},2000);
    var el2=(Date.now()-t0)/1000||0.001;
    showToast('\u2b07 Saved: '+name+' \u2014 '+fs(got)+' @ '+fs(got/el2)+'/s');
  }catch(e){
    if(e.name==='AbortError'){showToast('\u23f9 Cancelled: '+name);}
    else{alert('Download failed: '+e);}
  }
  finally{setBusy(false);navLock(false);dlCancelBtn(false);g_cancelFn=null;}
}
async function dlBlob(url,name){
  var btn=event&&event.target;if(btn)btn.disabled=true;
  setBusy(true,'\u23f3 Preparing ZIP: '+name);navLock(true);
  try{
    var r=await fetch(url);
    if(!r.ok){
      var d503=r.status===503?await r.json().catch(function(){return{};}):{};
      if(d503.op){alert('Device is busy ('+d503.op+'). Please wait and try again.');}
      else{alert('ZIP error: HTTP '+r.status);}
      return;
    }
    var d=await r.json();
    // If already done (rare fast path)
    if(d.ok){var zn=d.name||name;dlFetch('http://'+location.hostname+':8080/dl?path='+encodeURIComponent2(d.path)+'&name='+encodeURIComponent(zn),zn);return;}
    if(d.status!=='building'){alert('ZIP error: unexpected status');return;}
    // Poll /api/zip-status until done (max 60 × 1000 ms = 60 s)
    for(var i=0;i<60;i++){
      await new Promise(function(res){setTimeout(res,1000);});
      try{
        var sr=await fetch(H+'/api/zip-status');
        var sd=await sr.json();
        if(sd.ok){var zn2=sd.name||name;dlFetch('http://'+location.hostname+':8080/dl?path='+encodeURIComponent2(sd.path)+'&name='+encodeURIComponent(zn2),zn2);return;}
        if(sd.status!=='building'){alert('ZIP build failed');return;}
      }catch(pe){}
    }
    alert('ZIP build timed out');
  }catch(e){alert('Download failed: '+e);}
  finally{setBusy(false);navLock(false);if(btn)btn.disabled=false;}
}
async function rm(p){if(!confirm('Delete "'+p.split('/').pop()+'"?'))return;
var r=await fetch(H+'/api/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encodeURIComponent(p)});
var d=await r.json();if(d.ok){if(g_sm)searchAll();else loadDir(cp);loadLog();}else alert('Delete failed');}
async function rnm(fp,name){var nn=prompt('Rename to:',name);if(!nn||nn===name)return;
var dir=fp.substring(0,fp.lastIndexOf('/'));var to=(dir===''?'':dir)+'/'+nn;
var r=await fetch(H+'/api/rename',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'from='+encodeURIComponent(fp)+'&to='+encodeURIComponent(to)});
var d=await r.json();if(d.ok){if(g_sm)searchAll();else loadDir(cp);loadLog();}else alert('Rename failed');}
async function mkD(){var n=prompt('Folder name:');if(!n)return;
var fp=(cp==='/'?'':cp)+'/'+n;
setBusy(true,'\uD83D\uDCC1 Creating \u201C'+n+'\u201D\u2026');
var r=await fetch(H+'/api/mkdir',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encodeURIComponent(fp)});
var d=await r.json();setBusy(false);if(d.ok){showToast('\u2713 Folder created');loadDir(cp);loadLog();}else alert('mkdir failed');}
function updSel(){var all=document.querySelectorAll('.item-chk');var chk=document.querySelectorAll('.item-chk:checked');
document.getElementById('chkAll').checked=all.length>0&&chk.length===all.length;
document.getElementById('chkAll').indeterminate=chk.length>0&&chk.length<all.length;
document.getElementById('selcnt').textContent=chk.length+' selected';updSelBar();}
function updSelBar(){var n=document.querySelectorAll('.item-chk:checked').length;document.getElementById('selbar').style.display=n?'flex':'none';}
function selAll(v){document.querySelectorAll('.item-chk').forEach(function(c){c.checked=v;});updSel();}
function togSelAll(){var tot=document.querySelectorAll('.item-chk').length;var chk=document.querySelectorAll('.item-chk:checked').length;selAll(chk<tot);}
async function delSel(){var pp=[];document.querySelectorAll('.item-chk:checked').forEach(function(c){pp.push(c.dataset.path);});
if(!pp.length)return;if(!confirm('Delete '+pp.length+' item(s)?'))return;
for(var i=0;i<pp.length;i++){await fetch(H+'/api/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encodeURIComponent(pp[i])});}
if(g_sm)searchAll();else loadDir(cp);}
var g_upMode=0;
function setUpMode(m){g_upMode=m;document.getElementById('fiUpF').style.display=m===0?'':'none';document.getElementById('fiUpD').style.display=m===1?'':'none';var s0='background:#0cf;color:#000',s1='background:#1a1a1a;color:#aaa';document.getElementById('upMF').style.cssText='font-size:.75em;padding:3px 9px;'+(m===0?s0:s1);document.getElementById('upMD').style.cssText='font-size:.75em;padding:3px 9px;'+(m===1?s0:s1);}
function togUp(){var b=document.getElementById('upbox');b.style.display=b.style.display==='none'?'block':'none';}
function upXHR(file,path,fi,tot,pgEl,spdEl){return new Promise(function(res){var xhr=new XMLHttpRequest(),t0=Date.now();
g_cancelFn=function(){xhr.abort();};
xhr.upload.onprogress=function(e){if(!e.lengthComputable)return;var el=(Date.now()-t0)/1000||.001,sp=e.loaded/el,rm=(e.total-e.loaded)/sp;var pct=Math.round(e.loaded*100/e.total);pgEl.value=e.loaded;pgEl.max=e.total;var det=(fi+1)+'/'+tot+' \u00b7 '+fs(sp)+'/s \u00b7 '+ft(rm)+' left';spdEl.textContent=det;showToast('\u2b06 '+file.name+' \u2014 '+pct+'% \u00b7 '+fs(sp)+'/s \u00b7 ETA '+ft(rm),true);};
xhr.onload=function(){try{res(JSON.parse(xhr.responseText).ok);}catch(e){res(false);}};
xhr.onerror=function(){res(false);};
xhr.onabort=function(){res(null);};
var fp=(path===''?'':path)+'/'+file.name;xhr.open('POST',H+'/upload?path='+encodeURIComponent(fp));xhr.setRequestHeader('Content-Type','application/octet-stream');xhr.send(file);})}
async function doUp(){var fiEl=document.getElementById(g_upMode===0?'fiUpF':'fiUpD'),files=fiEl.files;if(!files.length)return;
var pgEl=document.getElementById('pgUp'),spdEl=document.getElementById('spdUp'),umEl=document.getElementById('umUp');
pgEl.style.display='block';spdEl.textContent='Starting\u2026';var failed=0,cancelled=false;
setBusy(true,'\u2b06 Uploading '+(files.length>1?files.length+' files':files[0].name)+'\u2026');navLock(true);dlCancelBtn(true);
for(var i=0;i<files.length;i++){var file=files[i],destPath;
if(g_upMode===1&&file.webkitRelativePath){var rp=file.webkitRelativePath,ls2=rp.lastIndexOf('/');destPath=(cp==='/'?'':cp)+(ls2>0?'/'+rp.substring(0,ls2):'');}
else{destPath=cp==='/'?'':cp;}var ok=await upXHR(file,destPath,i,files.length,pgEl,spdEl);if(ok===null){cancelled=true;break;}if(!ok)failed++;}
pgEl.style.display='none';spdEl.textContent='';setBusy(false);navLock(false);dlCancelBtn(false);g_cancelFn=null;
if(cancelled){showToast('\u23f9 Upload cancelled');loadDir(cp);loadLog();return;}
if(failed){umEl.textContent=failed+' file(s) failed';umEl.className='msg ng';}
else{umEl.textContent='Done! '+files.length+' file'+(files.length>1?'s':'')+' uploaded';umEl.className='msg ok';}
setTimeout(function(){umEl.className='msg';},4000);loadDir(cp);loadLog();}
async function dlSel(){
  var items=[];document.querySelectorAll('.item-chk:checked').forEach(function(c){items.push({path:c.dataset.path,dir:c.dataset.dir==='1'});});
  if(!items.length)return;
  if(items.length===1){
    var it=items[0],nm=it.path.split('/').pop()||'item';
    if(it.dir){dlBlob(H+'/download-zip?path='+encodeURIComponent(it.path)+'&name='+encodeURIComponent(nm+'.zip'),nm+'.zip');}
    else{dlFetch('http://'+location.hostname+':8080/dl?path='+encodeURIComponent2(it.path),nm);}
    return;
  }
  var paths=items.map(function(i){return i.path;}).join(',');
  var nm=(cp==='/'?'Root':cp.split('/').pop()||'Selection')+'_sel.zip';
  dlBlob(H+'/download-zip?paths='+encodeURIComponent(paths)+'&name='+encodeURIComponent(nm),nm);
}
var g_dlPoll=null,g_pollFails=0;
// dlCancelBtn: toast cancel button — used by dlFetch (file dl) and doUp (upload)
function dlCancelBtn(show){document.getElementById('cancelBtn').style.display=show?'':'none';}
// dlUrlCancelBtn: standalone cancel in download bar — used by URL downloads only
function dlUrlCancelBtn(show){document.getElementById('dlcancel').style.display=show?'':'none';}
var g_cancelFn=null;
function globalCancel(){if(g_cancelFn){var fn=g_cancelFn;g_cancelFn=null;fn();}}
function startDlPoll(){g_pollFails=0;clearInterval(g_dlPoll);g_dlPoll=setInterval(dlPoll,1000);}
async function dlUrl(){
  var url=document.getElementById('dlurl').value.trim();
  if(!url)return;
  if(!url.startsWith('http://')&&!url.startsWith('https://')){alert('URL must start with http:// or https://');return;}
  navLock(true);setBusy(true,'\u2b07 Queuing\u2026');
  try{
    var r=await fetch(H+'/api/download',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'url='+encodeURIComponent(url)});
    var d=await r.json();
    if(!r.ok||!d.ok){setBusy(false);navLock(false);alert('Error: '+(d.error||r.status));return;}
    document.getElementById('dlurl').value='';
    var qn=d.queue_count||1;
    showToast(qn>1?'\u23f3 Queued #'+qn+': '+(d.filename||'file'):'\u2b07 Downloading: '+(d.filename||'file'),true);
    dlUrlCancelBtn(true);startDlPoll();
  }catch(e){setBusy(false);navLock(false);alert('Request failed: '+e);}
}
async function dlPoll(){
  try{
    var r=await fetch(H+'/api/dl-status');var d=await r.json();
    g_pollFails=0;
    if(d.conflict){
      clearInterval(g_dlPoll);g_dlPoll=null;
      showCfModal(d.conflict);
    }else if(d.active){
      if(d.status==='connecting'){
        showToast('\u23f3 Connecting: '+(d.filename||'file')+'\u2026',true);
      }else{
      var sz=d.content_len>0?fs(d.bytes_recv)+' / '+fs(d.content_len):(d.bytes_recv>0?fs(d.bytes_recv):'');
      var spd=fspd(d.speed_kbps);
      var eta='';if(d.speed_kbps>0&&d.content_len>0&&d.bytes_recv<d.content_len){var rem=Math.round((d.content_len-d.bytes_recv)/(d.speed_kbps*1024));eta=ft(rem);}
      var parts=['\u2b07 '+(d.filename||'file')];if(sz)parts.push('Size: '+sz);if(spd)parts.push('Speed: '+spd);if(eta)parts.push('ETA: '+eta);
      if(d.queue_count>0)parts.push(d.queue_count+' more queued');
      showToast(parts.join(' \u2014 '),true);
      }
    }else{
      if(d.queue_count>0){showToast('\u23f3 '+d.queue_count+' more queued\u2026',true);return;}
      clearInterval(g_dlPoll);g_dlPoll=null;dlUrlCancelBtn(false);setBusy(false);navLock(false);
      if(d.status&&d.status!=='idle'){
        var ok=d.status==='done';
        var cancelled=d.status==='cancelled';
        showToast(ok?'\u2713 Done: '+(d.filename||''):cancelled?'\u23f9 Cancelled':(d.filename||'')+' \u2014 '+d.status);
        if(ok||cancelled||d.status==='skipped')setTimeout(function(){loadDir(cp);loadLog();},1200);else loadLog();
      }
    }
  }catch(e){if(++g_pollFails>5){clearInterval(g_dlPoll);g_dlPoll=null;dlUrlCancelBtn(false);setBusy(false);navLock(false);}}
}
async function dlCancel(){
  try{await fetch(H+'/api/dl-cancel',{method:'POST'});showToast('\u23f9 Cancelling\u2026',true);}catch(e){}
}
function showCfModal(name){document.getElementById('cfName').textContent=name;var m=document.getElementById('cfModal');m.style.display='flex';}
function hideCfModal(){document.getElementById('cfModal').style.display='none';}
async function dlResolve(action){
  hideCfModal();
  try{await fetch(H+'/api/dl-resolve',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action='+action});}catch(e){}
  startDlPoll();
}
var g_logOpen=false;
function toggleLog(){g_logOpen=!g_logOpen;document.getElementById('lgbody').style.display=g_logOpen?'block':'none';document.getElementById('lgtog').textContent=g_logOpen?'\u25b2':'\u25bc';if(g_logOpen)loadLog();}
async function loadLog(){if(!g_logOpen)return;try{var r=await fetch(H+'/api/log');var d=await r.json();var html=d.length?d.map(function(e){return '<div class="lge"><span class="lgts">'+ha(e.ts)+'</span><span class="lgop">'+ha(e.icon)+'</span><span class="lgfn">'+ha(e.name)+'</span><span class="lgdt">'+ha(e.detail)+'</span></div>';}).join(''):'<div style="color:#555;font-size:.75em;padding:4px">No activity yet</div>';document.getElementById('lgbody').innerHTML=html;}catch(e){}}
goTo('/');
(async function(){try{var r=await fetch(H+'/api/dl-status');var d=await r.json();if(d.active){navLock(true);setBusy(true,'\u2b07 '+(d.filename||'download')+' in progress',true);dlUrlCancelBtn(true);startDlPoll();}else if(d.conflict){showCfModal(d.conflict);}}catch(e){}})();
</script></body></html>
)html";

// Expose to dl_server.cpp without pulling ESPAsyncWebServer headers into it.
const char* web_server_fileman_html() { return FILEMAN_HTML; }

// ── Dashboard HTML (embedded, dark theme) ─────────────────────────────────────

static const char PAGE_HTML[] =
R"html(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>USB Smart Drive</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#0d0d0d;color:#eee;max-width:500px;margin:0 auto;padding:12px}
h1{color:#0cf;font-size:1.1em;padding:10px 0 2px}
.sub{color:#555;font-size:.75em;margin-bottom:14px}
.card{background:#141414;border:1px solid #1e1e1e;border-radius:8px;padding:14px;margin-bottom:12px}
.card h2{color:#0cf;font-size:.75em;font-weight:700;text-transform:uppercase;letter-spacing:.1em;margin-bottom:10px}
.row{display:flex;justify-content:space-between;align-items:center;padding:4px 0;font-size:.82em;border-bottom:1px solid #1a1a1a}
.row:last-child{border:0}
.lbl{color:#666}
.ok{color:#0f0}.err{color:#f44}.warn{color:#fa0}
label{display:block;font-size:.75em;color:#666;margin:9px 0 3px}
input,select{width:100%;padding:8px;background:#1c1c1c;border:1px solid #2a2a2a;border-radius:5px;color:#eee;font-size:.85em}
.btn{display:block;width:100%;padding:10px;border:none;border-radius:6px;font-size:.85em;font-weight:600;cursor:pointer;margin-top:8px}
.cyan{background:#0cf;color:#000}.cyan:active{background:#0af}
.red{background:#922;color:#fff}.red:active{background:#b33}
.themes{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:8px}
.th{padding:9px 4px;border-radius:5px;border:2px solid #2a2a2a;cursor:pointer;text-align:center;font-size:.8em;font-weight:600;transition:border-color .15s}
.th.sel{border-color:#0cf}
.th0{background:#000;color:#07ff}.th1{background:#001020;color:#5efe5e}.th2{background:#0a0500;color:#fd8000}
.msg{padding:6px 10px;border-radius:5px;font-size:.78em;margin-top:7px;display:none}
.msg.ok{background:#0a2a0a;color:#0f0;display:block}
.msg.ng{background:#2a0a0a;color:#f44;display:block}
</style></head><body>
<h1>&#128190; USB Smart Drive</h1>
<p class="sub" id="fw">Loading...</p>

<div class="card"><h2>Status</h2>
  <div class="row"><span class="lbl">Mode</span><span id="s-mode">--</span></div>
  <div class="row"><span class="lbl">WiFi</span><span id="s-wifi">--</span></div>
  <div class="row"><span class="lbl">IP Address</span><span id="s-ip">--</span></div>
  <div class="row"><span class="lbl">Storage</span><span id="s-sd">--</span></div>
</div>

<div class="card"><h2>File Manager</h2>
  <p style="font-size:.8em;color:#666;margin-bottom:8px">Browse, upload &amp; download files on the SD card.</p>
  <a href="/files" style="display:block"><button class="btn cyan" style="width:100%">Open File Manager</button></a>
</div>

<div class="card"><h2>Mode Switch</h2>
  <p style="font-size:.8em;color:#666;margin-bottom:6px">Active: <strong id="m-cur" style="color:#0cf">--</strong></p>
  <button class="btn cyan" id="m-btn" onclick="switchMode()">Switch Mode</button>
  <p style="font-size:.73em;color:#555;margin-top:8px">&#128274; In USB Drive Mode: double-click <code style="color:#fa0">Switch_to_Network_Mode.vbs</code> on the SD drive to switch automatically (no window).</p>
  <div class="msg" id="m-msg"></div>
</div>

<div class="card"><h2>WiFi Settings</h2>
  <label style="display:flex;justify-content:space-between;align-items:center"><span>Network</span><button type="button" class="btn cyan" style="padding:3px 9px;font-size:.72em;margin:0;width:auto" onclick="refreshNets()">&#8635; Refresh</button></label>
  <select id="w-ssid" onchange="g_wf=true"><option>Loading&#8230;</option></select>
  <label>Password</label>
  <input type="password" id="w-pass" placeholder="Leave blank for open networks" oninput="g_wf=true">
  <label>IP Mode</label>
  <select id="w-ipmode" onchange="togStatic(this.value);g_wf=true">
    <option value="0">Automatic</option>
    <option value="1">Static IP</option>
  </select>
  <div id="staticFields" style="display:none">
    <label>IP Address</label>
    <input type="text" id="w-ip" placeholder="e.g. 192.168.1.100" oninput="g_wf=true">
    <label>Subnet Mask</label>
    <input type="text" id="w-mask" placeholder="255.255.255.0" oninput="g_wf=true">
    <label>Gateway</label>
    <input type="text" id="w-gw" placeholder="e.g. 192.168.1.1" oninput="g_wf=true">
  </div>
  <button class="btn cyan" onclick="saveWifi()">Save &amp; Restart</button>
  <button class="btn red" onclick="resetWifi()">Reset WiFi Credentials</button>
  <div class="msg" id="w-msg"></div>
</div>

<div class="card"><h2>Theme</h2>
  <div class="themes">
    <div class="th th0" id="th0" onclick="applyTheme(0)">Dark</div>
    <div class="th th1" id="th1" onclick="applyTheme(1)">Ocean</div>
    <div class="th th2" id="th2" onclick="applyTheme(2)">Retro</div>
  </div>
  <div class="msg" id="th-msg"></div>
</div>

<div class="card"><h2>Auto-Pack Sync</h2>
  <p style="font-size:.8em;color:#666;margin-bottom:6px">Place <code style="color:#fa0">/_manifest.json</code> on the SD card to batch-download files. Already-present files are skipped.</p>
  <p style="font-size:.75em;color:#444;margin-bottom:8px">Format: <code style="color:#888">{"files":[{"url":"https://..."},{"url":"...","path":"/folder/file.ext"}]}</code></p>
  <button class="btn cyan" onclick="syncManifest()">&#8635; Sync All</button>
  <div class="msg" id="mn-msg"></div>
</div>

<div class="card"><h2>Access Control</h2>
  <p style="font-size:.8em;color:#666;margin-bottom:6px">Protect the web UI with a password. Leave both fields blank to remove protection.</p>
  <label>Username</label>
  <input type="text" id="a-user" placeholder="e.g. admin" autocomplete="username">
  <label>Password</label>
  <input type="password" id="a-pass" placeholder="Leave blank to disable auth" autocomplete="new-password">
  <button class="btn cyan" onclick="saveAuth()">Save Access Control</button>
  <p style="font-size:.73em;color:#555;margin-top:6px">&#128272; To recover a forgotten password: hold BOOT button 2s &#8594; resets WiFi &amp; password.</p>
  <div class="msg" id="a-msg"></div>
</div>

<script>
var d={},g_wf=false;
function toast(id,t,ok){var e=document.getElementById(id);e.textContent=t;e.className='msg '+(ok?'ok':'ng');setTimeout(function(){e.className='msg'},3500);}
async function api(url,data){var r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data).toString()});return r.json();}
function applyStatus(d){
  document.getElementById('fw').textContent='v'+d.fw+' \u2014 '+DEVICE_NAME;
  var mn=d.mode===1?'USB Drive':'Network';
  document.getElementById('s-mode').textContent=mn;
  document.getElementById('m-cur').textContent=mn;
  document.getElementById('m-btn').textContent='Switch to '+(d.mode===1?'Network':'USB Drive');
  var wi=document.getElementById('s-wifi');
  if(d.wifi_ok){wi.textContent=d.ssid||'Connected';wi.className='ok';}
  else if(d.ap_mode){wi.textContent='AP Mode (no router)';wi.className='warn';}
  else{wi.textContent='Disconnected';wi.className='err';}
  document.getElementById('s-ip').textContent=d.ip+(d.ip_mode===1?' (static)':'');
  document.getElementById('s-sd').innerHTML=d.sd_ok?'<span class="ok">'+d.sd_free.toFixed(1)+' / '+d.sd_total.toFixed(0)+' GB free</span>':'<span class="err">Not found</span>';
  [0,1,2].forEach(function(i){document.getElementById('th'+i).classList.toggle('sel',i===d.theme);});
  if(!g_wf){var im=String(d.ip_mode||0);document.getElementById('w-ipmode').value=im;togStatic(im);if(d.ip_mode===1){document.getElementById('w-ip').value=d.s_ip||'';document.getElementById('w-gw').value=d.s_gw||'';document.getElementById('w-mask').value=d.s_mask||'';}}
}
function applyScan(html){
  var sel=document.getElementById('w-ssid');var prev=sel.value;
  sel.innerHTML=html;
  for(var i=0;i<sel.options.length;i++){if(sel.options[i].value===prev){sel.selectedIndex=i;break;}}
}
async function loadInit(){
  try{var r=await fetch('/api/init');d=await r.json();applyStatus(d);if(d.scan_html)applyScan(d.scan_html);}
  catch(e){document.getElementById('fw').textContent='Device offline';}
}
async function load(){
  try{var r=await fetch('/api/status');d=await r.json();applyStatus(d);}
  catch(e){document.getElementById('fw').textContent='Device offline';}
}
async function loadNets(){
  try{var r=await fetch('/api/scan');applyScan(await r.text());}catch(e){}
}
async function refreshNets(){
  document.getElementById('w-ssid').innerHTML='<option>Scanning\u2026</option>';
  try{await fetch('/api/scan?start=1');}catch(e){return;}
  for(var i=0;i<7;i++){
    await new Promise(function(r){setTimeout(r,1000);});
    try{var r=await fetch('/api/scan');var h=await r.text();
    if(h.indexOf('Scanning')<0&&h.indexOf('Refresh')<0){applyScan(h);return;}}catch(e){}
  }
  try{var r2=await fetch('/api/scan');applyScan(await r2.text());}catch(e){}
}
async function switchMode(){
  var next=d.mode===1?0:1;
  toast('m-msg','Switching\u2026 device will restart in a moment.',true);
  await api('/api/mode',{mode:next});
}
function togStatic(v){document.getElementById('staticFields').style.display=v==='1'?'block':'none';}
async function saveWifi(){
  var s=document.getElementById('w-ssid').value;
  var pw=document.getElementById('w-pass').value;
  if(!s){toast('w-msg','Select a network first.',false);return;}
  var im=document.getElementById('w-ipmode').value;
  var data={ssid:s,pass:pw,ip_mode:im};
  if(im==='1'){
    data.s_ip=document.getElementById('w-ip').value.trim();
    data.s_gw=document.getElementById('w-gw').value.trim();
    data.s_mask=document.getElementById('w-mask').value.trim()||'255.255.255.0';
    if(!data.s_ip||!data.s_gw){toast('w-msg','Enter IP Address and Gateway for static IP.',false);return;}
  }
  toast('w-msg','Saving\u2026 device will restart.',true);
  await api('/api/wifi',data);
}
async function resetWifi(){
  if(!confirm('Reset WiFi credentials? The device will restart in AP config mode.'))return;
  toast('w-msg','Resetting\u2026 connect to USBDrive-Config.',true);
  await api('/api/wifi-reset',{});
}
async function applyTheme(i){
  [0,1,2].forEach(function(j){document.getElementById('th'+j).classList.toggle('sel',j===i);});
  var r=await api('/api/theme',{id:i});
  toast('th-msg',r.ok?'Theme applied!':'Error.',r.ok);
}
async function syncManifest(){
  var host=location.hostname;
  toast('mn-msg','Reading manifest\u2026',true);
  try{
    var r=await fetch('http://'+host+':8080/manifest-sync',{method:'POST'});
    var j=await r.json();
    if(j.ok) toast('mn-msg','Queued: '+j.queued+', Skipped: '+j.skipped+(j.queue_full?', Queue full: '+j.queue_full:''),true);
    else toast('mn-msg',j.error||'Error',false);
  }catch(e){toast('mn-msg','Cannot reach port 8080',false);}
}
async function saveAuth(){
  var u=document.getElementById('a-user').value.trim();
  var pw=document.getElementById('a-pass').value;
  if(pw===''&&u!==''){toast('a-msg','Enter a password or clear both fields.',false);return;}
  var r=await api('/api/auth',{user:u,pass:pw});
  if(r.ok){document.getElementById('a-user').value='';document.getElementById('a-pass').value='';}
  toast('a-msg',r.ok?(pw?'Password set. You will be prompted next request.':'Auth removed.'):'Error saving.',r.ok);
}
loadInit();setInterval(load,12000);setInterval(loadNets,30000);
</script>
)html";

// ── Route handlers ─────────────────────────────────────────────────────────────

static void handle_root(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    Serial.printf("[HTTP] GET / from %s\n", req->client()->remoteIP().toString().c_str());
    String page = PAGE_HTML;
    page.replace("DEVICE_NAME", "'" DEVICE_NAME "'");
    AsyncWebServerResponse* r = req->beginResponse(200, "text/html", page);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
}

static void handle_files_html(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    // FILEMAN_HTML (16 KB) is served from port 8080 (synchronous WebServer,
    // Core 1) where large responses always deliver reliably. ESPAsyncWebServer
    // (async_service_task, Core 0) intermittently truncates responses > ~8 KB.
    String host = req->host();
    req->redirect("http://" + host + ":8080/files");
}

static void handle_not_found(AsyncWebServerRequest* req) {
    Serial.printf("[HTTP] %s %s from %s\n",
                  req->methodToString(), req->url().c_str(),
                  req->client()->remoteIP().toString().c_str());
    if (wifi_is_ap_mode()) {
        // Android connectivity check — must return 204 No Content or Android
        // thinks there is internet and routes all browser traffic via LTE instead
        // of WiFi, making 192.168.4.1 unreachable from the browser.
        if (req->url().indexOf("generate_204") >= 0 ||
            req->url().indexOf("gen_204")      >= 0) {
            req->send(204, "text/plain", "");
            return;
        }
        // Windows NCSI — returning correct response lets Windows mark WiFi as
        // "internet available" (no yellow triangle) while still allowing the
        // user to navigate to 192.168.4.1 manually.
        if (req->url().indexOf("connecttest.txt") >= 0 ||
            req->url().indexOf("ncsi.txt")        >= 0) {
            req->send(200, "text/plain", "Microsoft Connect Test");
            return;
        }
        // iOS/macOS captive portal — redirect triggers the system captive browser.
        req->redirect("http://" AP_IP "/");
        return;
    }
    req->send(404, "text/plain", "Not found");
}

// Build status JSON document (shared by /api/status and /api/init)
static void fill_status_json(JsonDocument& doc) {
    // NVS fields (mode, ip_mode, static IP) only change after a save→restart cycle.
    // Cache them after first read so /api/status and /api/init never re-open NVS.
    static int    s_nv_mode    = -1;
    static int    s_nv_ip_mode = -1;
    static String s_nv_s_ip, s_nv_s_gw, s_nv_s_mask;

    if (s_nv_mode < 0) {
        Preferences p;
        p.begin(NVS_NS, true);
        s_nv_mode = (int)p.getUChar(NVS_KEY_MODE, (uint8_t)MODE_NETWORK);
        p.end();

        Preferences wP;
        wP.begin("wifi", true);
        s_nv_ip_mode = (int)wP.getUChar("ip_mode", 0);
        s_nv_s_ip    = wP.getString("s_ip",   "");
        s_nv_s_gw    = wP.getString("s_gw",   "");
        s_nv_s_mask  = wP.getString("s_mask",  "255.255.255.0");
        wP.end();
    }

    doc["mode"]    = s_nv_mode;
    doc["fw"]      = FW_VERSION;
    doc["wifi_ok"] = wifi_connected();
    doc["ap_mode"] = wifi_is_ap_mode();
    doc["ip"]      = wifi_ip();
    doc["ssid"]    = wifi_ssid();
    doc["sd_ok"]   = storage_is_ready();
    doc["sd_free"] = storage_free_gb();
    doc["sd_total"]= storage_total_gb();
    doc["theme"]   = (int)theme_current_id();
    doc["ip_mode"] = s_nv_ip_mode;
    doc["s_ip"]    = s_nv_s_ip;
    doc["s_gw"]    = s_nv_s_gw;
    doc["s_mask"]  = s_nv_s_mask;
}

static void handle_status(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    JsonDocument doc;
    fill_status_json(doc);
    String json;
    serializeJson(doc, json);
    send_json(req, 200, json);
}

// /api/init — status + WiFi scan in one round-trip (saves one HTTP connection)
static void handle_init(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    // Never trigger a scan on page load — in both modes the user clicks ↻ Refresh.
    JsonDocument doc;
    fill_status_json(doc);
    // Show "click Refresh" placeholder until the user has run at least one scan.
    if (g_scan_cache.isEmpty()) {
        doc["scan_html"] = "<option value=''>\u21BB Click Refresh to scan\u2026</option>";
    } else {
        doc["scan_html"] = get_networks_html();
    }
    String json;
    serializeJson(doc, json);
    send_json(req, 200, json);
}

static void handle_scan(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    // Scan is triggered ONLY when the browser explicitly passes ?start=1
    // (the Refresh button). Without start=1 this is read-only (returns cache).
    // This way setInterval(loadNets) never starts a scan automatically in AP mode.
    if (req->hasParam("start") && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
        if (WiFi.scanComplete() >= 0) WiFi.scanDelete();  // discard stale results
        WiFi.scanNetworks(true);
    }
    req->send(200, "text/html", get_networks_html());
}

static void handle_busy(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    if (s_busy) {
        String j = "{\"busy\":true,\"op\":\""; j += s_busy_op; j += "\"}";
        send_json(req, 200, j);
    } else {
        send_json(req, 200, "{\"busy\":false,\"op\":\"\"}");
    }
}

static void handle_mode(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    String mode_s = qparam(req, "mode");
    int mode = mode_s.toInt();
    if (mode != (int)MODE_NETWORK && mode != (int)MODE_USB_DRIVE) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"invalid mode\"}");
        return;
    }
    Preferences p;
    p.begin(NVS_NS, false);
    p.putUChar(NVS_KEY_MODE, (uint8_t)mode);
    p.end();
    lcd_invalidate_layout();
    lcd_splash_msg("Switching...");
    send_json(req, 200, "{\"ok\":true}");
    g_restart_at = millis() + 800;
}

static void handle_wifi(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    String ssid = qparam(req, "ssid");
    if (ssid.isEmpty()) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"ssid required\"}");
        return;
    }
    Preferences p;
    p.begin("wifi", false);
    p.putString("ssid", ssid);
    p.putString("pass", qparam(req, "pass"));
    uint8_t ip_mode = (uint8_t)qparam(req, "ip_mode").toInt();
    p.putUChar("ip_mode", ip_mode);
    if (ip_mode == 1) {
        String s_mask = qparam(req, "s_mask");
        if (s_mask.isEmpty()) s_mask = "255.255.255.0";
        p.putString("s_ip",   qparam(req, "s_ip"));
        p.putString("s_gw",   qparam(req, "s_gw"));
        p.putString("s_mask", s_mask);
        p.putString("s_dns",  "8.8.8.8");
    }
    p.end();
    send_json(req, 200, "{\"ok\":true}");
    g_restart_at = millis() + 800;
}

static void handle_wifi_reset(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    Preferences p;
    p.begin("wifi", false);
    p.clear();
    p.end();
    send_json(req, 200, "{\"ok\":true}");
    g_restart_at = millis() + 800;
}

static void handle_theme(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    int id = qparam(req, "id").toInt();
    if (id < 0 || (uint8_t)id >= theme_count()) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"invalid theme\"}");
        return;
    }
    theme_save((uint8_t)id);
    lcd_apply_theme();
    send_json(req, 200, "{\"ok\":true}");
}

// ── File Manager helpers ───────────────────────────────────────────────────────

// Returns true for OS-generated system entries that should be hidden everywhere
// ── Path safety guard ─────────────────────────────────────────────────────────
// All user-supplied SD paths must be absolute and free of ".." components.
static bool safe_path(const String& p) {
    return p.startsWith("/") && p.indexOf("..") < 0;
}

// Percent-encode characters that are unsafe in a URL query-string value.
static String url_encode(const String& s) {
    String out;
    out.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/' || c == '~') {
            out += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            out += buf;
        }
    }
    return out;
}

// (listing, search, ZIP) — Windows volume metadata, recycle bin, macOS artifacts,
// and our own internal temp file.
static bool is_system_entry(const String& name) {
    if (name.isEmpty()) return true;
    // Windows: starts with '$' (e.g. $RECYCLE.BIN) or exact system folder names
    if (name[0] == '$') return true;
    if (name.equalsIgnoreCase("System Volume Information")) return true;
    // macOS: resource forks and spotlight/trash metadata
    if (name.startsWith("._"))         return true;
    if (name.equalsIgnoreCase(".Trashes"))        return true;
    if (name.equalsIgnoreCase(".Spotlight-V100")) return true;
    if (name.equalsIgnoreCase(".fseventsd"))      return true;
    // Our own temp file
    if (name == "_dl_tmp.zip") return true;
    return false;
}

static void mkdirs(const String& path) {
    for (int i = 1; i < (int)path.length(); i++) {
        if (path[i] == '/') SD_MMC.mkdir(path.substring(0, i));
    }
    if (path.length() > 1) SD_MMC.mkdir(path);
}


// Shared 16 KB read buffer for ZIP pass1/pass2 — protected by busy flag
static uint8_t s_dl_buf[16384];

// handle_download: ALL file downloads redirect to port 8080 synchronous server.
// Previously, small files (≤ 64 KB) were pre-read into SRAM here (async_service_task),
// but fread() DMA in async_service_task intermittently blocks/hangs on IDF 5.x,
// stalling async_service_task and making the entire web server unresponsive.
// Redirecting everything to port 8080 means zero SD I/O in async_service_task.
static void handle_download(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    if (!storage_is_ready()) {
        send_json(req, 503, "{\"error\":\"SD not ready\"}"); return;
    }
    String path = qparam(req, "path");
    if (path.isEmpty() || !safe_path(path)) { req->send(400, "text/plain", "invalid path"); return; }
    // Strip port from Host header so the redirect goes to the bare IP/hostname.
    String host = req->host();
    int cp = host.lastIndexOf(':');
    if (cp > 0) host = host.substring(0, cp);
    req->redirect("http://" + host + ":8080/dl?path=" + url_encode(path));
}

// ── ZIP folder download ───────────────────────────────────────────────────────
// Two-pass via SD temp file: pass 1 reads each file to compute CRC and record
// actual byte count; pass 2 writes the ZIP to /_dl_tmp.zip, streaming headers
// and data from SD. Avoids data-descriptor mode (confuses 7-Zip on binary files
// containing the PK\x07\x08 signature) and avoids backward seek in write mode
// (unreliable on ESP32 FatFS).

struct ZipEntry {
    String   zip_name;
    String   sd_path;
    uint32_t size;      // actual bytes read in pass 1
    uint32_t crc;       // CRC-32 computed in pass 1
    uint32_t hdr_offset;
};

#define ZIP_TMP "/_dl_tmp.zip"

static std::vector<ZipEntry> s_zip_entries;

static void zip_collect(const String& sdp, const String& pre) {
    File dir = SD_MMC.open(sdp);
    if (!dir || !dir.isDirectory()) { dir.close(); return; }
    File f = dir.openNextFile();
    while (f) {
        yield();
        // ESP32 Core 2.x openNextFile().name() returns basename only (no directory
        // prefix). Build the absolute path explicitly from the parent directory path.
        const char* raw = f.name();
        String fn;
        if (raw && raw[0] == '/') {
            fn = String(raw);  // already absolute
        } else {
            fn = sdp;
            if (!fn.endsWith("/")) fn += '/';
            fn += raw;
        }
        int sl = fn.lastIndexOf('/');
        String base = sl >= 0 ? fn.substring(sl + 1) : fn;
        // Skip system entries and our own temp file
        if (base.length() > 0 && !is_system_entry(base)) {
            String zn = pre.isEmpty() ? base : (pre + "/" + base);
            if (f.isDirectory()) zip_collect(fn, zn);
            else s_zip_entries.push_back({zn, fn, 0, 0, 0});
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
}

// Pass 1: read each file to compute CRC and record exact byte count.
// Using actual bytes read (not f.size()) avoids header/data mismatch if
// the FAT directory entry size is stale or wrong.
static void zip_pass1_crc() {
    for (auto& e : s_zip_entries) {
        File f = SD_MMC.open(e.sd_path, FILE_READ);
        uint32_t state = 0xFFFFFFFF;
        uint32_t actual = 0;
        if (f) {
            f.setBufferSize(16384);
            int n;
            while ((n = f.read(s_dl_buf, sizeof(s_dl_buf))) > 0) {
                state = crc32_feed(state, s_dl_buf, (size_t)n);
                actual += (uint32_t)n;
                yield();
            }
            f.close();
        }
        e.crc  = state ^ 0xFFFFFFFF;
        e.size = actual;
    }
}

// SD temp-file ZIP builder.
// ZIP is assembled on SD (/_dl_tmp.zip) — no RAM buffer needed.
static File g_zw_fd;

// Returns false on write failure — caller must abort and call fail().
static bool zw(const void* d, size_t n) {
    return g_zw_fd.write((const uint8_t*)d, n) == n;
}
static bool zw16(uint16_t v) { uint8_t b[2]={uint8_t(v),uint8_t(v>>8)}; return zw(b,2); }
static bool zw32(uint32_t v) { uint8_t b[4]={uint8_t(v),uint8_t(v>>8),uint8_t(v>>16),uint8_t(v>>24)}; return zw(b,4); }

static void zw_local_hdr(const String& name, uint32_t crc, uint32_t sz) {
    uint16_t nl = name.length();
    zw32(0x04034b50); zw16(20); zw16(0x0800);
    zw16(0); zw16(0); zw16(0);
    zw32(crc); zw32(sz); zw32(sz);
    zw16(nl); zw16(0);
    zw(name.c_str(), nl);
}
static void zw_central(const ZipEntry& e) {
    uint16_t nl = e.zip_name.length();
    zw32(0x02014b50); zw16(20); zw16(20); zw16(0x0800);
    zw16(0); zw16(0); zw16(0);
    zw32(e.crc); zw32(e.size); zw32(e.size);
    zw16(nl); zw16(0); zw16(0); zw16(0); zw16(0);
    zw32(0); zw32(e.hdr_offset);
    zw(e.zip_name.c_str(), nl);
}

// run_deferred_zip — called from web_server_loop() (Core 1 Arduino loop task).
// Sets g_zip_state / g_zip_done_json; JS polls /api/zip-status for the result.
static void run_deferred_zip() {
    String path = g_zip_req_path;
    bool   isSel = (g_zip_sel_paths[0] != '\0');
    String selPaths;
    if (isSel) { selPaths = g_zip_sel_paths; g_zip_sel_paths[0] = '\0'; }

    auto fail = [&]() {
        if (g_zw_fd) { g_zw_fd.close(); SD_MMC.remove(ZIP_TMP); }
        s_zip_entries.clear();
        g_zip_state = ZIP_STATE_ERROR;
        busy_clear();
    };

    if (!storage_is_ready()) { fail(); return; }

    // ── Collect file list ──────────────────────────────────────────────────────
    s_zip_entries.clear();
    if (isSel) {
        // Parse comma-separated paths; add files directly, recurse into dirs.
        int start = 0;
        while (start < (int)selPaths.length()) {
            int comma = selPaths.indexOf(',', start);
            String p = (comma < 0) ? selPaths.substring(start) : selPaths.substring(start, comma);
            p.trim();
            if (p.length() > 1 && safe_path(p)) {
                int sl = p.lastIndexOf('/');
                String base = sl >= 0 ? p.substring(sl + 1) : p;
                if (!is_system_entry(base)) {
                    File f = SD_MMC.open(p.c_str());
                    if (f) {
                        if (f.isDirectory()) zip_collect(p, base);
                        else                 s_zip_entries.push_back({base, p, 0, 0, 0});
                        f.close();
                    }
                }
            }
            if (comma < 0) break;
            start = comma + 1;
        }
    } else {
        String pre;
        if (path != "/") {
            int sl = path.lastIndexOf('/');
            pre = sl >= 0 ? path.substring(sl + 1) : path;
        }
        zip_collect(path, pre);
    }

    if (s_zip_entries.empty()) { fail(); return; }

    // ── Pass 1: CRC + actual byte count ────────────────────────────────────────
    zip_pass1_crc();

    // ── Pass 2: write ZIP to SD temp file ──────────────────────────────────────
    SD_MMC.remove(ZIP_TMP);
    g_zw_fd = SD_MMC.open(ZIP_TMP, FILE_WRITE);
    if (!g_zw_fd) { fail(); return; }

    bool zip_write_ok = true;
    for (auto& e : s_zip_entries) {
        e.hdr_offset = (uint32_t)g_zw_fd.position();
        zw_local_hdr(e.zip_name, e.crc, e.size);

        File f = SD_MMC.open(e.sd_path, FILE_READ);
        if (f) {
            f.setBufferSize(16384);
            uint32_t written = 0;
            while (written < e.size) {
                uint32_t chunk = min((uint32_t)sizeof(s_dl_buf), e.size - written);
                int n = f.read(s_dl_buf, chunk);
                if (n <= 0) break;
                if (!zw(s_dl_buf, (size_t)n)) { zip_write_ok = false; break; }
                written += (uint32_t)n;
                yield();
            }
            f.close();
        }
        if (!zip_write_ok) break;
    }
    if (!zip_write_ok) { fail(); return; }

    // ── Write central directory + EOCD ─────────────────────────────────────────
    uint32_t cd_off = (uint32_t)g_zw_fd.position();
    for (const auto& e : s_zip_entries) zw_central(e);
    uint32_t cd_sz  = (uint32_t)g_zw_fd.position() - cd_off;
    uint16_t cnt    = (uint16_t)s_zip_entries.size();
    zw32(0x06054b50); zw16(0); zw16(0);
    zw16(cnt); zw16(cnt);
    zw32(cd_sz); zw32(cd_off); zw16(0);

    g_zw_fd.close();
    s_zip_entries.clear();

    String fname;
    if (isSel) {
        // Name was passed as ?name= from JS; fall back to "Selection.zip"
        fname = strlen(g_zip_req_path) > 0 ? String(g_zip_req_path) : String("Selection.zip");
    } else {
        fname = (path == "/") ? "sd_root" : path.substring(path.lastIndexOf('/') + 1);
        fname += ".zip";
    }
    if (fname.length() > 60) fname = fname.substring(0, 60);

    snprintf(g_zip_done_json, sizeof(g_zip_done_json),
             "{\"ok\":true,\"path\":\"%s\",\"name\":\"%s\"}", ZIP_TMP, fname.c_str());
    g_zip_state = ZIP_STATE_DONE;  // written AFTER json — acts as release barrier
    busy_clear();
}

// handle_download_zip — responds immediately; JS polls /api/zip-status.
static void handle_download_zip(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    if (!busy_try("zip")) { send_busy(req); return; }
    if (!storage_is_ready()) {
        busy_clear();
        req->send(503, "text/plain", "SD not ready"); return;
    }

    if (req->hasParam("paths")) {
        // Multi-select mode: comma-separated SD paths
        String paths = qparam(req, "paths");
        if (paths.isEmpty()) { busy_clear(); send_json(req, 400, "{\"error\":\"empty paths\"}"); return; }
        strncpy(g_zip_sel_paths, paths.c_str(), sizeof(g_zip_sel_paths) - 1);
        g_zip_sel_paths[sizeof(g_zip_sel_paths) - 1] = '\0';
        // Store the requested zip filename in g_zip_req_path for naming
        String name = qparam(req, "name");
        strncpy(g_zip_req_path, name.c_str(), 255);
        g_zip_req_path[255] = '\0';
    } else {
        // Single directory mode
        String path = qparam(req, "path");
        if (path.isEmpty()) path = "/";
        if (!safe_path(path)) { busy_clear(); send_json(req, 400, "{\"error\":\"invalid path\"}"); return; }
        strncpy(g_zip_req_path, path.c_str(), 255);
        g_zip_req_path[255] = '\0';
        g_zip_sel_paths[0] = '\0';
    }

    g_zip_state     = ZIP_STATE_BUILDING;
    g_zip_requested = true;
    req->send(200, "application/json", "{\"status\":\"building\"}");
}

// /api/zip-status — JS polls this while ZIP is building.
static void handle_zip_status(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    if (g_zip_state == ZIP_STATE_DONE) {
        // Ensure g_zip_done_json is fully visible before clearing state (Xtensa LX7 has
        // relaxed memory ordering for volatile; explicit barrier matches Core 1 release).
        __asm__ volatile("" ::: "memory");
        g_zip_state = ZIP_STATE_IDLE;
        req->send(200, "application/json", g_zip_done_json);
    } else if (g_zip_state == ZIP_STATE_ERROR) {
        __asm__ volatile("" ::: "memory");
        g_zip_state = ZIP_STATE_IDLE;
        req->send(500, "application/json", "{\"ok\":false,\"error\":\"zip build failed\"}");
    } else if (g_zip_state == ZIP_STATE_BUILDING) {
        req->send(200, "application/json", "{\"status\":\"building\"}");
    } else {
        req->send(200, "application/json", "{\"status\":\"idle\"}");
    }
}

// ── Upload (raw binary body, path as query param) ─────────────────────────────

struct UploadCtx {
    File     file;
    bool     ok  = false;
    bool     busy_held = false;
    uint32_t write_fail_bytes = 0;
};

static void handle_upload(AsyncWebServerRequest* req) {
    // Called after all body chunks are received.
    if (!auth_check(req)) {
        // Auth failed: clean up any partial upload that was set up in handle_upload_body
        UploadCtx* ctx = req->_tempObject ? (UploadCtx*)req->_tempObject : nullptr;
        if (ctx) {
            if (ctx->file) ctx->file.close();
            if (ctx->busy_held) busy_clear();
            delete ctx;
            req->_tempObject = nullptr;
        }
        g_upload_lcd_active = false;
        return;
    }
    UploadCtx* ctx = req->_tempObject ? (UploadCtx*)req->_tempObject : nullptr;
    bool ok = ctx && ctx->ok;
    uint32_t fail_bytes = ctx ? ctx->write_fail_bytes : 0;
    String fname;
    if (ctx) {
        if (ctx->file) { fname = ctx->file.name(); ctx->file.close(); }
        if (ctx->busy_held) busy_clear();
        delete ctx;
        req->_tempObject = nullptr;
    }
    // Clear upload LCD (Core 1 will see this and stop showing progress)
    g_upload_lcd_active = false;

    if (fname.length()) {
        String bname = fname.substring(fname.lastIndexOf('/') + 1);
        if (ok) {
            // Detail: file size
            char detail[48];
            uint32_t sz = g_upload_lcd_total;
            if (sz >= 1048576) snprintf(detail, sizeof(detail), "%.1f MB from PC", sz / 1048576.0f);
            else               snprintf(detail, sizeof(detail), "%d KB from PC", (int)(sz / 1024));
            actlog_add(ACT_UPLOAD, bname.c_str(), detail);
        } else {
            uint32_t got = g_upload_lcd_bytes;
            char detail[64];
            if (fail_bytes > 0)  snprintf(detail, sizeof(detail), "Write err %u B lost @ %d KB", fail_bytes, (int)(got / 1024));
            else if (got >= 1024) snprintf(detail, sizeof(detail), "Failed @ %d KB", (int)(got / 1024));
            else                 strncpy(detail, "Upload failed", sizeof(detail));
            actlog_add(ACT_WARN, bname.c_str(), detail);
        }
    }
    if (fail_bytes > 0) {
        char errbuf[80];
        snprintf(errbuf, sizeof(errbuf),
                 "{\"ok\":false,\"error\":\"write failed\",\"lost_bytes\":%u}", fail_bytes);
        send_json(req, 500, errbuf);
    } else {
        send_json(req, ok ? 200 : 500,
                  ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"upload failed\"}");
    }
}

static void handle_upload_body(AsyncWebServerRequest* req,
                               uint8_t* data, size_t len,
                               size_t index, size_t total) {
    if (index == 0) {
        // First chunk — set up context
        if (!busy_try("upload")) {
            req->_tempObject = nullptr;
            return;
        }
        String path = qparam(req, "path");
        if (path.isEmpty() || !safe_path(path) || !storage_is_ready()) {
            busy_clear();
            req->_tempObject = nullptr;
            return;
        }
        int last_slash = path.lastIndexOf('/');
        if (last_slash > 0) mkdirs(path.substring(0, last_slash));

        if (total > 2UL * 1024 * 1024 * 1024) {
            busy_clear();
            req->_tempObject = nullptr;
            return;
        }
        UploadCtx* ctx = new UploadCtx();
        ctx->busy_held = true;
        ctx->file = SD_MMC.open(path, FILE_WRITE);
        ctx->ok   = (bool)ctx->file;
        req->_tempObject = ctx;

        // Signal Core 1 to start showing upload LCD
        String bname = path.substring(path.lastIndexOf('/') + 1);
        strncpy(g_upload_lcd_name, bname.c_str(), sizeof(g_upload_lcd_name) - 1);
        g_upload_lcd_name[sizeof(g_upload_lcd_name) - 1] = '\0';
        g_upload_lcd_bytes  = 0;
        g_upload_lcd_total  = (uint32_t)total;
        g_upload_lcd_active = true;   // Core 1 picks this up in web_server_loop()
    }

    UploadCtx* ctx = req->_tempObject ? (UploadCtx*)req->_tempObject : nullptr;
    if (ctx && ctx->file && len > 0) {
        size_t w = ctx->file.write(data, len);
        if (w != len) {
            ctx->write_fail_bytes += (uint32_t)(len - w);
            ctx->ok = false;
        }
        // Update byte counter for LCD (Core 1 reads this)
        g_upload_lcd_bytes = (uint32_t)(index + len);
    }
}

static void handle_delete(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    String path = qparam(req, "path");
    if (path.isEmpty() || !safe_path(path)) { send_json(req, 400, "{\"ok\":false}"); return; }
    bool ok = SD_MMC.remove(path) || SD_MMC.rmdir(path);
    if (ok) actlog_add(ACT_DEL, path.c_str());
    send_json(req, 200, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handle_mkdir(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    String path = qparam(req, "path");
    if (path.isEmpty() || !safe_path(path)) { send_json(req, 400, "{\"ok\":false}"); return; }
    bool ok = SD_MMC.mkdir(path);
    if (ok) actlog_add(ACT_MKDIR, path.c_str());
    send_json(req, 200, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handle_rename(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    String from = qparam(req, "from");
    String to   = qparam(req, "to");
    if (from.isEmpty() || to.isEmpty() || !safe_path(from) || !safe_path(to)) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"params required\"}"); return;
    }
    bool ok = SD_MMC.rename(from, to);
    if (ok) {
        String detail = "\u2192 " + to.substring(to.lastIndexOf('/') + 1);
        actlog_add(ACT_RENAME, from.substring(from.lastIndexOf('/') + 1).c_str(), detail.c_str());
    }
    send_json(req, 200, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rename failed\"}");
}


// ── URL downloader routes ─────────────────────────────────────────────────────

static void handle_api_download(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    String url = qparam(req, "url");
    if (url.isEmpty()) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"url required\"}");
        return;
    }
    if (!url.startsWith("http://") && !url.startsWith("https://")) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"url must start with http or https\"}");
        return;
    }
    if (!storage_is_ready()) {
        send_json(req, 503, "{\"ok\":false,\"error\":\"SD not ready\"}");
        return;
    }
    if (!downloader_queue(url.c_str())) {
        send_json(req, 503, "{\"ok\":false,\"error\":\"queue full\"}");
        return;
    }
    String fn = downloader_quick_filename(url.c_str());
    String resp = "{\"ok\":true,\"status\":\"queued\",\"filename\":\"" + fn +
                  "\",\"queue_count\":" + String(downloader_queue_count()) + "}";
    send_json(req, 200, resp);
}

static void handle_dl_status(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    JsonDocument doc;
    doc["active"]       = downloader_is_busy();
    doc["progress"]     = downloader_progress();
    doc["filename"]     = downloader_filename();
    doc["status"]       = downloader_status();
    doc["conflict"]     = downloader_conflict_pending() ? downloader_conflict_name() : "";
    doc["speed_kbps"]   = downloader_speed();
    doc["bytes_recv"]   = downloader_bytes_recv();
    doc["content_len"]  = downloader_content_len();
    doc["queue_count"]  = downloader_queue_count();
    String json;
    serializeJson(doc, json);
    send_json(req, 200, json);
}

static void handle_dl_cancel(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    downloader_cancel();
    send_json(req, 200, "{\"ok\":true}");
}

static void handle_dl_resolve(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    String action = qparam(req, "action");
    if (action != "replace" && action != "skip" && action != "cancel") {
        send_json(req, 400, "{\"ok\":false,\"error\":\"invalid action\"}");
        return;
    }
    downloader_resolve(action.c_str());
    send_json(req, 200, "{\"ok\":true}");
}

static void handle_log(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;
    String json = actlog_get_json();
    send_json(req, 200, json);
}

static void handle_auth(AsyncWebServerRequest* req) {
    if (!auth_check(req)) return;   // must be authed to change credentials
    String user = qparam(req, "user");
    String pass = qparam(req, "pass");
    Preferences p;
    p.begin(NVS_NS, false);
    if (pass.isEmpty()) {
        p.remove(NVS_KEY_AUTH_USER);
        p.remove(NVS_KEY_AUTH_PASS);
    } else {
        p.putString(NVS_KEY_AUTH_USER, user);
        p.putString(NVS_KEY_AUTH_PASS, pass);
    }
    p.end();
    auth_cache_invalidate();   // also invalidates dl_server cache via dl_auth_cache_invalidate()
    send_json(req, 200, "{\"ok\":true}");
}

// ── Public API ─────────────────────────────────────────────────────────────────

void web_server_begin() {
    server.on("/",               HTTP_GET,  handle_root);
    server.on("/files",          HTTP_GET,  handle_files_html);
    server.on("/api/status",     HTTP_GET,  handle_status);
    server.on("/api/init",       HTTP_GET,  handle_init);
    server.on("/api/busy",       HTTP_GET,  handle_busy);
    server.on("/api/scan",       HTTP_GET,  handle_scan);
    server.on("/api/mode",       HTTP_POST, handle_mode);
    server.on("/api/wifi",       HTTP_POST, handle_wifi);
    server.on("/api/wifi-reset", HTTP_POST, handle_wifi_reset);
    server.on("/api/theme",      HTTP_POST, handle_theme);
    server.on("/download",       HTTP_GET,  handle_download);
    server.on("/download-zip",   HTTP_GET,  handle_download_zip);
    server.on("/api/zip-status", HTTP_GET,  handle_zip_status);
    server.on("/api/delete",     HTTP_POST, handle_delete);
    server.on("/api/mkdir",      HTTP_POST, handle_mkdir);
    server.on("/api/rename",     HTTP_POST, handle_rename);
    server.on("/api/download",   HTTP_POST, handle_api_download);
    server.on("/api/dl-status",  HTTP_GET,  handle_dl_status);
    server.on("/api/dl-cancel",  HTTP_POST, handle_dl_cancel);
    server.on("/api/dl-resolve", HTTP_POST, handle_dl_resolve);
    server.on("/api/log",        HTTP_GET,  handle_log);
    server.on("/api/auth",       HTTP_POST, handle_auth);
    // Upload: completion handler + body handler (raw binary, no multipart)
    server.on("/upload", HTTP_POST, handle_upload, nullptr, handle_upload_body);
    // CORS preflight: Chrome sends OPTIONS before non-simple cross-origin requests
    // (e.g. POST /upload with Content-Type: application/octet-stream from port 8080
    // page).  Returning 404 blocks the upload entirely — must respond 204 + headers.
    server.onNotFound([](AsyncWebServerRequest* req) {
        if (req->method() == HTTP_OPTIONS) {
            req->send(204, "text/plain", "");
            return;
        }
        handle_not_found(req);
    });
    // CORS: allow the file manager page (served from port 8080) to call
    // all port-80 API endpoints (delete, rename, mkdir, upload, download, etc.)
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

    server.begin();
    Serial.println("[HTTP] AsyncWebServer started on port 80");

    // Port 8080: synchronous file/ZIP download (dl_server.cpp).
    dl_server_begin();
}

void web_server_loop() {
    // ── Deferred ZIP build ─────────────────────────────────────────────────────
    // Runs here (Core 1 loop task) instead of async_service_task so that
    // async_service_task remains free to serve new port-80 requests while building.
    if (g_zip_requested) {
        g_zip_requested = false;
        run_deferred_zip();
    }

    // Busy watchdog: if a heavy operation stalls (dropped connection) for >30 s,
    // free any in-flight ZIP buffer and clear the busy flag.
    static uint32_t s_busy_since = 0;
    if (s_busy) {
        if (s_busy_since == 0) s_busy_since = millis();
        else if (millis() - s_busy_since > 15000UL) {
            busy_clear();
            s_busy_since = 0;
        }
    } else {
        s_busy_since = 0;
    }

    // Deferred restart: handler sets g_restart_at = millis() + delay.
    // The main loop polls here so the HTTP response is fully sent first.
    if (g_restart_at > 0 && millis() >= g_restart_at) {
        ESP.restart();
    }

    // ── Upload LCD — driven here (Core 1) from volatile state set by Core 0 ──────
    static bool     s_upload_was_active = false;
    static uint32_t s_upload_last_lcd   = 0;
    if (g_upload_lcd_active) {
        if (!s_upload_was_active) {
            // Transition: idle → active
            g_upload_lcd_start  = millis();
            s_upload_last_lcd   = 0;
            s_upload_was_active = true;
            lcd_invalidate_layout();
        }
        uint32_t now = millis();
        if (now - s_upload_last_lcd >= 300) {
            uint32_t got   = g_upload_lcd_bytes;
            uint32_t total = g_upload_lcd_total;
            uint8_t  pct   = (total > 0) ? (uint8_t)((uint64_t)got * 100 / total) : 0;
            uint32_t el    = now - g_upload_lcd_start;
            int      spd   = (el > 200) ? (int)((int64_t)got * 1000 / el / 1024) : -1;
            lcd_show_progress(String(g_upload_lcd_name), pct, spd, (int32_t)got,
                              (int)total, 0, "PC Upload");
            s_upload_last_lcd = now;
        }
    } else if (s_upload_was_active) {
        // Transition: active → done
        s_upload_was_active = false;
        lcd_invalidate_layout();
    }

    // Port 8080 synchronous download server — streamFile() runs here (Core 1).
    dl_server_loop();
}
