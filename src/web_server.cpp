#include "web_server.h"
#include "config.h"
#include "wifi_manager.h"
#include "storage.h"
#include "themes.h"
#include "lcd.h"
#include "downloader.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <vector>
#include <ESPAsyncWebServer.h>

// ── Server instance (plain HTTP port 80) ──────────────────────────────────────

static AsyncWebServer server(80);

// Non-zero = restart at this millis() timestamp (after response is sent).
static uint32_t g_restart_at = 0;

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

// ── CRC-32/ISO-HDLC (ZIP standard) ────────────────────────────────────────────
// Pure software nibble-table — no ROM function calling-convention ambiguity.
// Verified test vector: crc32_feed(0xFFFFFFFF,"123456789",9)^0xFFFFFFFF == 0xCBF43926
// Usage:
//   uint32_t state = 0xFFFFFFFF;
//   state = crc32_feed(state, buf, n);   // for each chunk
//   uint32_t crc = state ^ 0xFFFFFFFF;   // finalize
static uint32_t crc32_feed(uint32_t state, const uint8_t* buf, size_t len) {
    static const uint32_t T[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
        0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
    };
    for (size_t i = 0; i < len; i++) {
        state = T[(state ^ buf[i]) & 0xF] ^ (state >> 4);
        state = T[(state ^ (buf[i] >> 4)) & 0xF] ^ (state >> 4);
    }
    return state;
}

// ── Cached WiFi scan — manual only in all modes ───────────────────────────────
// No automatic background scanning in either AP or STA mode.
// The ↻ Refresh button sends /api/scan?start=1 which triggers one scan.
// handle_scan returns the cached result immediately; the browser JS polls
// every 1 s via refreshNets() until fresh results arrive (~2-3 s scan time).

static String   g_scan_cache;
static uint32_t g_scan_ts         = 0;
static uint32_t g_scan_trigger_ts = 0;  // 0 = triggers immediately on first loop

static String get_networks_html() {
    int n = WiFi.scanComplete();  // -2=running, -1=idle, >=0=results
    if (n >= 0) {
        g_scan_cache = "";
        for (int i = 0; i < n; i++) {
            String s = WiFi.SSID(i);
            g_scan_cache += "<option value=\"" + s + "\">"
                          + s + " (" + WiFi.RSSI(i) + " dBm)</option>";
        }
        if (g_scan_cache.isEmpty())
            g_scan_cache = "<option value=''>No networks found</option>";
        g_scan_ts = millis();
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
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Files</title><style>
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
.toast{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);background:#1a1a2e;border:1px solid #0cf;border-radius:8px;color:#0cf;padding:8px 16px;font-size:.82em;z-index:999;display:none;max-width:90vw;text-align:center;box-shadow:0 4px 16px #000a}
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
</style></head><body>
<div class="toast" id="toast"></div>
<h1>&#128190; File Manager <a href="/">&#8592; Config</a></h1>
<div class="bc" id="bread"></div>
<div class="card">
<div class="bar">
  <button class="btn c" id="bk" onclick="goBack()" disabled>&#8592;</button>
  <button class="btn c" id="fw" onclick="goFwd()" disabled>&#8594;</button>
  <button class="btn c" onclick="tup(0)">&#8679; Files</button>
  <button class="btn c" onclick="tup(1)">&#128193; Folder</button>
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
  <button class="btn" id="dlcancel" style="display:none;background:#400;color:#f88" onclick="dlCancel()">&#10005;</button>
</div>
<div class="selbar" id="selbar">
  <input type="checkbox" class="chk" id="chkAll" onchange="selAll(this.checked)">
  <span id="selcnt">0 selected</span>
  <button class="btn rd" onclick="delSel()">&#128465; Delete Selected</button>
</div>
<div class="srhdr" id="srhdr"></div>
<div id="upbox0" style="display:none;margin-bottom:10px">
  <input class="uf" type="file" id="fi0" multiple>
  <button class="btn c" onclick="doUp(0)">Start Upload</button>
  <progress id="pg0"></progress><div class="spd" id="spd0"></div>
  <div class="msg" id="um0"></div>
</div>
<div id="upbox1" style="display:none;margin-bottom:10px">
  <input class="uf" type="file" id="fi1" webkitdirectory>
  <button class="btn c" onclick="doUp(1)">Start Upload</button>
  <progress id="pg1"></progress><div class="spd" id="spd1"></div>
  <div class="msg" id="um1"></div>
</div>
<div id="ls"></div>
</div>
<script>
var cp='/',hist=['/'],hIdx=0,g_ent=[],g_sort=0,g_sm=false,g_ent_bk=[],g_st=null,g_busy=0;
function showToast(msg){var t=document.getElementById('toast');t.textContent=msg;t.style.display=msg?'block':'none';}
function setBusy(on,msg){g_busy=Math.max(0,g_busy+(on?1:-1));if(g_busy>0){showToast(msg||'\u23f3 Working\u2026');}else{showToast('');}}
function navLock(on){document.getElementById('bk').disabled=on||(hIdx===0);document.getElementById('fw').disabled=on||(hIdx>=hist.length-1);}
var SORTS=['&#8645; Name','&#8645; Name&#8595;','&#8645; Size&#8595;'];
function fs(n){return n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':n<1073741824?(n/1048576).toFixed(1)+' MB':(n/1073741824).toFixed(2)+' GB';}
function ft(s){if(s<1)return '<1s';return s<60?Math.ceil(s)+'s':Math.floor(s/60)+'m '+Math.ceil(s%60)+'s';}
function isImg(n){return /\.(jpe?g|png|gif|webp|bmp|svg)$/i.test(n);}
function updNav(){if(g_busy>0)return;document.getElementById('bk').disabled=hIdx===0;document.getElementById('fw').disabled=hIdx>=hist.length-1;}
function setBread(p){var b='<a href="#" onclick="goTo(\'/\')">root</a>';var pts=p.split('/').filter(Boolean),a='';pts.forEach(function(x){a+='/'+x;var pa=a;b+=' / <a href="#" onclick="goTo(\''+je(pa)+'\')">'+x+'</a>';});document.getElementById('bread').innerHTML=b;}
function goTo(p){if(g_sm)clrSrch();hist=hist.slice(0,hIdx+1);if(hist[hIdx]!==p){hist.push(p);hIdx=hist.length-1;}loadDir(p);updNav();}
function goBack(){if(hIdx>0){hIdx--;loadDir(hist[hIdx]);updNav();}}
function goFwd(){if(hIdx<hist.length-1){hIdx++;loadDir(hist[hIdx]);updNav();}}
function togSort(){g_sort=(g_sort+1)%3;document.getElementById('sortBtn').innerHTML=SORTS[g_sort];renderDir();}
function onSrch(){var q=document.getElementById('srch').value;document.getElementById('srchX').style.display=q?'':'none';clearTimeout(g_st);if(q.trim()){g_st=setTimeout(searchAll,500);}else{if(g_sm){g_sm=false;g_ent=g_ent_bk;document.getElementById('srhdr').style.display='none';}renderDir();}}
function clrSrch(){clearTimeout(g_st);document.getElementById('srch').value='';document.getElementById('srchX').style.display='none';if(g_sm){g_sm=false;g_ent=g_ent_bk;document.getElementById('srhdr').style.display='none';}renderDir();}
async function searchAll(){var q=document.getElementById('srch').value.trim();if(!q)return;document.getElementById('ls').innerHTML='<div style="color:#555;padding:8px;font-size:.8em">Searching&#8230;</div>';
try{var r=await fetch('/api/search?q='+encodeURIComponent(q));var d=await r.json();
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
h+='<div class="row"><input type="checkbox" class="chk item-chk" data-path="'+ha(fp)+'" onchange="updSel()"><span>&#128193;</span>';
h+='<span class="nm"><a href="#" class="fn" onclick="goTo(\''+je(fp)+'\')">'+e.name+'</a>';
if(g_sm&&e.parent){h+='<span class="np">'+ha(e.parent)+'</span>';}
h+='</span>';
h+='<button class="ren" onclick="rnm(\''+je(fp)+'\',\''+je(e.name)+'\')" title="Rename">&#9998;</button>';
h+='<button class="dld" onclick="dlBlob(\'/download-zip?path='+fpe+'\',\''+je(e.name)+'.zip\')" title="ZIP">&#8659;zip</button>';
h+='<button class="act del" onclick="rm(\''+je(fp)+'\')">&#128465;</button></div>';}
else{
h+='<div class="row"><input type="checkbox" class="chk item-chk" data-path="'+ha(fp)+'" onchange="updSel()"><span>&#128196;</span>';
h+='<span class="nm"><a href="#" class="fn'+(isImg(e.name)?' img':'')+'" onclick="dlBlob(\'/download?path='+fpe+'\',\''+je(e.name)+'\')">'+e.name+'</a>';
if(g_sm&&e.parent){h+='<span class="np">'+ha(e.parent)+'</span>';}
h+='</span>';
h+='<span class="sz">'+fs(e.size||0)+'</span>';
h+='<button class="ren" onclick="rnm(\''+je(fp)+'\',\''+je(e.name)+'\')" title="Rename">&#9998;</button>';
h+='<button class="dld" onclick="dlBlob(\'/download?path='+fpe+'\',\''+je(e.name)+'\')" title="Download">&#8659;</button>';
h+='<button class="act del" onclick="rm(\''+je(fp)+'\')">&#128465;</button></div>';}
});document.getElementById('ls').innerHTML=h||'<div style="color:#555;padding:8px;font-size:.8em">'+(g_sm?'No results':'Empty folder')+'</div>';updSelBar();}
async function loadDir(p){cp=p;setBread(p);document.getElementById('ls').innerHTML='<div style="color:#555;padding:8px;font-size:.8em">Loading&#8230;</div>';
try{var r=await fetch('/api/list?path='+encodeURIComponent(p));var d=await r.json();
if(d.free_gb!=null)document.getElementById('stor').textContent='Free: '+d.free_gb.toFixed(1)+'/'+d.total_gb.toFixed(0)+' GB';
g_ent=d.entries||[];renderDir();}catch(ex){document.getElementById('ls').innerHTML='<div style="color:#f44;padding:8px;font-size:.8em">Load error</div>';}}
function encodeURIComponent2(p){return encodeURIComponent(p).replace(/'/g,'%27');}
function je(s){return s.replace(/\\/g,'\\\\').replace(/'/g,"\\'");}
function ha(s){return s.replace(/&/g,'&amp;').replace(/"/g,'&quot;');}
async function dlBlob(url,name){
  var btn=event&&event.target;if(btn)btn.disabled=true;
  var isZip=url.indexOf('download-zip')>=0;
  var label=isZip?'\u23f3 Preparing ZIP: '+name:'\u2b07 Downloading: '+name;
  setBusy(true,label);navLock(true);
  try{
    var r=await fetch(url);
    if(!r.ok){
      var d503=r.status===503?await r.json().catch(function(){return{};}):{};
      if(d503.op){alert('Device is busy ('+d503.op+'). Please wait and try again.');}
      else{alert('Download error: HTTP '+r.status);}
      return;
    }
    var blob=await r.blob();
    var burl=URL.createObjectURL(blob);
    var a=document.createElement('a');a.href=burl;a.download=name;
    document.body.appendChild(a);a.click();document.body.removeChild(a);
    setTimeout(function(){URL.revokeObjectURL(burl);},60000);
  }catch(e){alert('Download failed: '+e);}
  finally{setBusy(false);navLock(false);if(btn)btn.disabled=false;}
}
async function rm(p){if(!confirm('Delete "'+p.split('/').pop()+'"?'))return;
var r=await fetch('/api/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encodeURIComponent(p)});
var d=await r.json();if(d.ok){if(g_sm)searchAll();else loadDir(cp);}else alert('Delete failed');}
async function rnm(fp,name){var nn=prompt('Rename to:',name);if(!nn||nn===name)return;
var dir=fp.substring(0,fp.lastIndexOf('/'));var to=(dir===''?'':dir)+'/'+nn;
var r=await fetch('/api/rename',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'from='+encodeURIComponent(fp)+'&to='+encodeURIComponent(to)});
var d=await r.json();if(d.ok){if(g_sm)searchAll();else loadDir(cp);}else alert('Rename failed');}
async function mkD(){var n=prompt('Folder name:');if(!n)return;
var fp=(cp==='/'?'':cp)+'/'+n;
setBusy(true,'\uD83D\uDCC1 Creating \u201C'+n+'\u201D\u2026');
var r=await fetch('/api/mkdir',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encodeURIComponent(fp)});
var d=await r.json();setBusy(false);if(d.ok){showToast('\u2713 Folder created');loadDir(cp);}else alert('mkdir failed');}
function updSel(){var all=document.querySelectorAll('.item-chk');var chk=document.querySelectorAll('.item-chk:checked');
document.getElementById('chkAll').checked=all.length>0&&chk.length===all.length;
document.getElementById('chkAll').indeterminate=chk.length>0&&chk.length<all.length;
document.getElementById('selcnt').textContent=chk.length+' selected';updSelBar();}
function updSelBar(){var n=document.querySelectorAll('.item-chk:checked').length;document.getElementById('selbar').style.display=n?'flex':'none';}
function selAll(v){document.querySelectorAll('.item-chk').forEach(function(c){c.checked=v;});updSel();}
function togSelAll(){var tot=document.querySelectorAll('.item-chk').length;var chk=document.querySelectorAll('.item-chk:checked').length;selAll(chk<tot);}
async function delSel(){var pp=[];document.querySelectorAll('.item-chk:checked').forEach(function(c){pp.push(c.dataset.path);});
if(!pp.length)return;if(!confirm('Delete '+pp.length+' item(s)?'))return;
for(var i=0;i<pp.length;i++){await fetch('/api/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encodeURIComponent(pp[i])});}
if(g_sm)searchAll();else loadDir(cp);}
function tup(i){var b0=document.getElementById('upbox0'),b1=document.getElementById('upbox1');if(i===0){b0.style.display=b0.style.display==='none'?'block':'none';b1.style.display='none';}else{b1.style.display=b1.style.display==='none'?'block':'none';b0.style.display='none';}}
function upXHR(file,path,fi,tot,pgEl,spdEl){return new Promise(function(res){var xhr=new XMLHttpRequest(),t0=Date.now();
xhr.upload.onprogress=function(e){if(!e.lengthComputable)return;var el=(Date.now()-t0)/1000||.001,sp=e.loaded/el,rm=(e.total-e.loaded)/sp;pgEl.value=e.loaded;pgEl.max=e.total;spdEl.textContent=(fi+1)+'/'+tot+' \u00b7 '+fs(sp)+'/s \u00b7 '+ft(rm)+' left';};
xhr.onload=function(){try{res(JSON.parse(xhr.responseText).ok);}catch(e){res(false);}};
xhr.onerror=function(){res(false);};
var fp=(path===''?'':path)+'/'+file.name;xhr.open('POST','/upload?path='+encodeURIComponent(fp));xhr.setRequestHeader('Content-Type','application/octet-stream');xhr.send(file);})}
async function doUp(mode){var fiEl=document.getElementById('fi'+mode),files=fiEl.files;if(!files.length)return;
var pgEl=document.getElementById('pg'+mode),spdEl=document.getElementById('spd'+mode),umEl=document.getElementById('um'+mode);
pgEl.style.display='block';spdEl.textContent='Starting\u2026';var failed=0;
setBusy(true,'\u2b06 Uploading '+(files.length>1?files.length+' files':files[0].name)+'\u2026');navLock(true);
for(var i=0;i<files.length;i++){var file=files[i],destPath;
if(mode===1&&file.webkitRelativePath){var rp=file.webkitRelativePath,ls2=rp.lastIndexOf('/');destPath=(cp==='/'?'':cp)+(ls2>0?'/'+rp.substring(0,ls2):'');}
else{destPath=cp==='/'?'':cp;}var ok=await upXHR(file,destPath,i,files.length,pgEl,spdEl);if(!ok)failed++;}
pgEl.style.display='none';spdEl.textContent='';setBusy(false);navLock(false);
if(failed){umEl.textContent=failed+' file(s) failed';umEl.className='msg ng';}
else{umEl.textContent='Done! '+files.length+' file'+(files.length>1?'s':'')+' uploaded';umEl.className='msg ok';}
setTimeout(function(){umEl.className='msg';},4000);loadDir(cp);}
var g_dlPoll=null;
function dlCancelBtn(show){document.getElementById('dlcancel').style.display=show?'':'none';}
function startDlPoll(){clearInterval(g_dlPoll);g_dlPoll=setInterval(dlPoll,2000);}
async function dlUrl(){
  var url=document.getElementById('dlurl').value.trim();
  if(!url)return;
  if(!url.startsWith('http://')&&!url.startsWith('https://')){alert('URL must start with http:// or https://');return;}
  navLock(true);setBusy(true,'\u2b07 Queuing\u2026');
  try{
    var r=await fetch('/api/download',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'url='+encodeURIComponent(url)});
    var d=await r.json();
    if(!r.ok||!d.ok){setBusy(false);navLock(false);alert('Error: '+(d.error||r.status));return;}
    document.getElementById('dlurl').value='';
    showToast('\u2b07 Downloading: '+(d.filename||'file'));
    dlCancelBtn(true);startDlPoll();
  }catch(e){setBusy(false);navLock(false);alert('Request failed: '+e);}
}
async function dlPoll(){
  try{
    var r=await fetch('/api/dl-status');var d=await r.json();
    if(d.active){
      showToast('\u2b07 '+(d.filename||'file')+' \u2014 '+d.status);
    }else{
      clearInterval(g_dlPoll);g_dlPoll=null;dlCancelBtn(false);setBusy(false);navLock(false);
      if(d.status&&d.status!=='idle'){
        var ok=d.status==='done';
        var cancelled=d.status==='cancelled';
        showToast(ok?'\u2713 Done: '+(d.filename||''):cancelled?'\u23f9 Cancelled':(d.filename||'')+' \u2014 '+d.status);
        if(ok)setTimeout(function(){loadDir(cp);},1200);
      }
    }
  }catch(e){clearInterval(g_dlPoll);g_dlPoll=null;dlCancelBtn(false);setBusy(false);navLock(false);}
}
async function dlCancel(){
  try{await fetch('/api/dl-cancel',{method:'POST'});showToast('\u23f9 Cancelling\u2026');}catch(e){}
}
goTo('/');
(async function(){try{var r=await fetch('/api/dl-status');var d=await r.json();if(d.active){navLock(true);setBusy(true,'\u2b07 '+(d.filename||'download')+' in progress');dlCancelBtn(true);startDlPoll();}}catch(e){}})();
</script></body></html>
)html";

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
  <p style="font-size:.73em;color:#555;margin-top:8px">&#128274; In USB Drive Mode: double-click <code style="color:#fa0">Switch_to_Network_Mode.bat</code> on the SD drive to switch automatically.</p>
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
loadInit();setInterval(load,12000);setInterval(loadNets,30000);
</script>
)html";

// ── Route handlers ─────────────────────────────────────────────────────────────

static void handle_root(AsyncWebServerRequest* req) {
    Serial.printf("[HTTP] GET / from %s\n", req->client()->remoteIP().toString().c_str());
    String page = PAGE_HTML;
    page.replace("DEVICE_NAME", "'" DEVICE_NAME "'");
    AsyncWebServerResponse* r = req->beginResponse(200, "text/html", page);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
}

static void handle_files_html(AsyncWebServerRequest* req) {
    AsyncWebServerResponse* r = req->beginResponse(200, "text/html", FILEMAN_HTML);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
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
    Preferences p;
    p.begin(NVS_NS, true);
    doc["mode"] = (int)p.getUChar(NVS_KEY_MODE, (uint8_t)MODE_NETWORK);
    p.end();

    doc["fw"]       = FW_VERSION;
    doc["wifi_ok"]  = wifi_connected();
    doc["ap_mode"]  = wifi_is_ap_mode();
    doc["ip"]       = wifi_ip();
    doc["ssid"]     = wifi_ssid();
    doc["sd_ok"]    = storage_is_ready();
    doc["sd_free"]  = storage_free_gb();
    doc["sd_total"] = storage_total_gb();
    doc["theme"]    = (int)theme_current_id();

    Preferences wP;
    wP.begin("wifi", true);
    doc["ip_mode"] = (int)wP.getUChar("ip_mode", 0);
    doc["s_ip"]    = wP.getString("s_ip",   "");
    doc["s_gw"]    = wP.getString("s_gw",   "");
    doc["s_mask"]  = wP.getString("s_mask",  "255.255.255.0");
    wP.end();
}

static void handle_status(AsyncWebServerRequest* req) {
    JsonDocument doc;
    fill_status_json(doc);
    String json;
    serializeJson(doc, json);
    send_json(req, 200, json);
}

// /api/init — status + WiFi scan in one round-trip (saves one HTTP connection)
static void handle_init(AsyncWebServerRequest* req) {
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
    if (s_busy) {
        String j = "{\"busy\":true,\"op\":\""; j += s_busy_op; j += "\"}";
        send_json(req, 200, j);
    } else {
        send_json(req, 200, "{\"busy\":false,\"op\":\"\"}");
    }
}

static void handle_mode(AsyncWebServerRequest* req) {
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
    Preferences p;
    p.begin("wifi", false);
    p.clear();
    p.end();
    send_json(req, 200, "{\"ok\":true}");
    g_restart_at = millis() + 800;
}

static void handle_theme(AsyncWebServerRequest* req) {
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

static void handle_list(AsyncWebServerRequest* req) {
    if (!storage_is_ready()) { send_json(req, 503, "{\"error\":\"SD not ready\"}"); return; }
    String path = qparam(req, "path");
    if (path.isEmpty()) path = "/";

    File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        send_json(req, 404, "{\"error\":\"not a directory\"}");
        return;
    }

    JsonDocument doc;
    doc["path"]     = path;
    doc["free_gb"]  = storage_free_gb();
    doc["total_gb"] = storage_total_gb();
    JsonArray arr   = doc["entries"].to<JsonArray>();

    File entry = dir.openNextFile();
    while (entry) {
        String fullname = entry.name();
        int slash = fullname.lastIndexOf('/');
        String name = slash >= 0 ? fullname.substring(slash + 1) : fullname;
        if (name.length() > 0 && !is_system_entry(name)) {
            JsonObject obj = arr.add<JsonObject>();
            obj["name"] = name;
            obj["dir"]  = entry.isDirectory();
            obj["size"] = entry.isDirectory() ? 0 : (uint32_t)entry.size();
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    String json;
    serializeJson(doc, json);
    send_json(req, 200, json);
}

// Shared 8 KB read buffer (only used during download/ZIP — protected by busy flag)
static uint8_t s_dl_buf[8192];

static void handle_download(AsyncWebServerRequest* req) {
    if (!busy_try("download")) { send_busy(req); return; }
    if (!storage_is_ready()) {
        busy_clear();
        send_json(req, 503, "{\"error\":\"SD not ready\"}"); return;
    }
    String path = qparam(req, "path");
    if (path.isEmpty()) {
        busy_clear();
        req->send(400, "text/plain", "path required"); return;
    }
    File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        busy_clear();
        req->send(404, "text/plain", "Not found"); return;
    }
    String name = path.substring(path.lastIndexOf('/') + 1);
    // beginResponse(File, name_for_mime, contentType, download=true)
    // download=true adds Content-Disposition: attachment automatically.
    AsyncWebServerResponse* resp = req->beginResponse(
        f, name, "application/octet-stream", true);
    resp->addHeader("Cache-Control", "no-cache");
    req->send(resp);
    busy_clear();
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

static void zw(const void* d, size_t n) { g_zw_fd.write((const uint8_t*)d, n); }
static void zw16(uint16_t v) { uint8_t b[2]={uint8_t(v),uint8_t(v>>8)}; zw(b,2); }
static void zw32(uint32_t v) { uint8_t b[4]={uint8_t(v),uint8_t(v>>8),uint8_t(v>>16),uint8_t(v>>24)}; zw(b,4); }

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

static void handle_download_zip(AsyncWebServerRequest* req) {
    if (!busy_try("zip")) { send_busy(req); return; }
    if (!storage_is_ready()) {
        busy_clear();
        req->send(503, "text/plain", "SD not ready"); return;
    }
    String path = qparam(req, "path");
    if (path.isEmpty()) path = "/";

    s_zip_entries.clear();
    String pre;
    if (path != "/") {
        int sl = path.lastIndexOf('/');
        pre = sl >= 0 ? path.substring(sl + 1) : path;
    }
    zip_collect(path, pre);

    if (s_zip_entries.empty()) {
        busy_clear();
        req->send(404, "text/plain", "Folder is empty or not found"); return;
    }

    // Pass 1: CRC + actual byte count (reads each file once)
    zip_pass1_crc();

    // Pass 2: build ZIP on SD temp file
    SD_MMC.remove(ZIP_TMP);
    g_zw_fd = SD_MMC.open(ZIP_TMP, FILE_WRITE);
    if (!g_zw_fd) {
        busy_clear();
        s_zip_entries.clear();
        req->send(500, "text/plain", "Cannot create temp file on SD"); return;
    }

    for (auto& e : s_zip_entries) {
        e.hdr_offset = (uint32_t)g_zw_fd.position();
        zw_local_hdr(e.zip_name, e.crc, e.size);

        File f = SD_MMC.open(e.sd_path, FILE_READ);
        if (f) {
            uint32_t written = 0;
            while (written < e.size) {
                uint32_t chunk = min((uint32_t)sizeof(s_dl_buf), e.size - written);
                int n = f.read(s_dl_buf, chunk);
                if (n <= 0) break;
                zw(s_dl_buf, (size_t)n);
                written += (uint32_t)n;
                yield();
            }
            f.close();
        }
    }

    uint32_t cd_off = (uint32_t)g_zw_fd.position();
    for (const auto& e : s_zip_entries) zw_central(e);
    uint32_t cd_sz  = (uint32_t)g_zw_fd.position() - cd_off;
    uint16_t cnt    = (uint16_t)s_zip_entries.size();
    zw32(0x06054b50); zw16(0); zw16(0);
    zw16(cnt); zw16(cnt);
    zw32(cd_sz); zw32(cd_off); zw16(0);

    g_zw_fd.close();
    s_zip_entries.clear();

    String fname = (path == "/") ? "sd_root" : path.substring(path.lastIndexOf('/') + 1);
    fname += ".zip";

    File zf = SD_MMC.open(ZIP_TMP, FILE_READ);
    if (!zf) {
        busy_clear();
        SD_MMC.remove(ZIP_TMP);
        req->send(500, "text/plain", "Cannot open ZIP temp file"); return;
    }

    AsyncWebServerResponse* resp = req->beginResponse(zf, fname, "application/zip", true);
    resp->addHeader("Cache-Control", "no-cache");
    req->send(resp);
    busy_clear();
}

// ── Upload (raw binary body, path as query param) ─────────────────────────────

struct UploadCtx {
    File     file;
    bool     ok  = false;
    bool     busy_held = false;
};

static void handle_upload(AsyncWebServerRequest* req) {
    // Called after all body chunks are received.
    UploadCtx* ctx = req->_tempObject ? (UploadCtx*)req->_tempObject : nullptr;
    bool ok = ctx && ctx->ok;
    if (ctx) {
        if (ctx->file) ctx->file.close();
        if (ctx->busy_held) busy_clear();
        delete ctx;
        req->_tempObject = nullptr;
    }
    send_json(req, ok ? 200 : 500,
              ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"upload failed\"}");
}

static void handle_upload_body(AsyncWebServerRequest* req,
                               uint8_t* data, size_t len,
                               size_t index, size_t total) {
    if (index == 0) {
        // First chunk — set up context
        if (!busy_try("upload")) {
            // Store null so completion handler sends 503
            req->_tempObject = nullptr;
            return;
        }
        String path = qparam(req, "path");
        if (path.isEmpty() || !storage_is_ready()) {
            busy_clear();
            req->_tempObject = nullptr;
            return;
        }
        int last_slash = path.lastIndexOf('/');
        if (last_slash > 0) mkdirs(path.substring(0, last_slash));

        // Reject uploads declared larger than 2 GB (SD limit is ~32 GB but
        // prevent trivial disk-exhaustion if client lies about Content-Length)
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
    }

    UploadCtx* ctx = req->_tempObject ? (UploadCtx*)req->_tempObject : nullptr;
    if (ctx && ctx->file && len > 0) {
        ctx->file.write(data, len);
    }
}

static void handle_delete(AsyncWebServerRequest* req) {
    String path = qparam(req, "path");
    if (path.isEmpty()) { send_json(req, 400, "{\"ok\":false}"); return; }
    bool ok = SD_MMC.remove(path) || SD_MMC.rmdir(path);
    send_json(req, 200, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handle_mkdir(AsyncWebServerRequest* req) {
    String path = qparam(req, "path");
    if (path.isEmpty()) { send_json(req, 400, "{\"ok\":false}"); return; }
    send_json(req, 200, SD_MMC.mkdir(path) ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handle_rename(AsyncWebServerRequest* req) {
    String from = qparam(req, "from");
    String to   = qparam(req, "to");
    if (from.isEmpty() || to.isEmpty()) {
        send_json(req, 400, "{\"ok\":false,\"error\":\"params required\"}"); return;
    }
    bool ok = SD_MMC.rename(from, to);
    send_json(req, 200, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rename failed\"}");
}

// ── Global search ──────────────────────────────────────────────────────────────

static void search_walk(const String& sdp, const String& q, JsonArray arr, int& cnt, int depth = 0) {
    if (cnt >= 200 || depth > 20) return;  // depth limit prevents stack overflow
    File dir = SD_MMC.open(sdp);
    if (!dir || !dir.isDirectory()) { dir.close(); return; }
    File f = dir.openNextFile();
    while (f && cnt < 200) {
        yield();
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
                if (!f.isDirectory()) obj["size"] = (uint32_t)f.size();
                cnt++;
            }
            if (f.isDirectory()) search_walk(fn, q, arr, cnt, depth + 1);
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
}

static void handle_search(AsyncWebServerRequest* req) {
    if (!busy_try("search")) { send_busy(req); return; }
    if (!storage_is_ready()) { busy_clear(); send_json(req, 503, "{\"error\":\"SD not ready\"}"); return; }
    String q = qparam(req, "q");
    q.toLowerCase();
    if (q.isEmpty()) { busy_clear(); send_json(req, 400, "{\"error\":\"q required\"}"); return; }
    JsonDocument doc;
    JsonArray arr = doc["entries"].to<JsonArray>();
    doc["free_gb"]  = storage_free_gb();
    doc["total_gb"] = storage_total_gb();
    int cnt = 0;
    search_walk("/", q, arr, cnt);
    busy_clear();
    String json;
    serializeJson(doc, json);
    send_json(req, 200, json);
}

// ── URL downloader routes ─────────────────────────────────────────────────────

static void handle_api_download(AsyncWebServerRequest* req) {
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
        send_json(req, 503, "{\"ok\":false,\"error\":\"already downloading\"}");
        return;
    }
    String fn = downloader_quick_filename(url.c_str());
    String resp = "{\"ok\":true,\"status\":\"queued\",\"filename\":\"" + fn + "\"}";
    send_json(req, 200, resp);
}

static void handle_dl_status(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["active"]   = downloader_is_busy();
    doc["progress"] = downloader_progress();
    doc["filename"] = downloader_filename();
    doc["status"]   = downloader_status();
    String json;
    serializeJson(doc, json);
    send_json(req, 200, json);
}

static void handle_dl_cancel(AsyncWebServerRequest* req) {
    downloader_cancel();
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
    server.on("/api/list",       HTTP_GET,  handle_list);
    server.on("/api/search",     HTTP_GET,  handle_search);
    server.on("/download",       HTTP_GET,  handle_download);
    server.on("/download-zip",   HTTP_GET,  handle_download_zip);
    server.on("/api/delete",     HTTP_POST, handle_delete);
    server.on("/api/mkdir",      HTTP_POST, handle_mkdir);
    server.on("/api/rename",     HTTP_POST, handle_rename);
    server.on("/api/download",   HTTP_POST, handle_api_download);
    server.on("/api/dl-status",  HTTP_GET,  handle_dl_status);
    server.on("/api/dl-cancel",  HTTP_POST, handle_dl_cancel);
    // Upload: completion handler + body handler (raw binary, no multipart)
    server.on("/upload", HTTP_POST, handle_upload, nullptr, handle_upload_body);
    server.onNotFound(handle_not_found);
    server.begin();
    Serial.println("[HTTP] Server started on port 80");
}

void web_server_loop() {
    // Busy watchdog: if a heavy operation stalls (dropped connection) for >30 s,
    // free any in-flight ZIP buffer and clear the busy flag.
    static uint32_t s_busy_since = 0;
    if (s_busy) {
        if (s_busy_since == 0) s_busy_since = millis();
        else if (millis() - s_busy_since > 30000UL) {
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
}
