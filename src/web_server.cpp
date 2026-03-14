#include "web_server.h"
#include "config.h"
#include "wifi_manager.h"
#include "storage.h"
#include "themes.h"
#include "lcd.h"
#include "ssl_certs.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <vector>
#include <HTTPSServer.hpp>
#include <HTTPServer.hpp>
#include <SSLCert.hpp>
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <ResourceNode.hpp>

using namespace httpsserver;

// ── Server instances ──────────────────────────────────────────────────────────

static SSLCert     g_cert((unsigned char*)SSL_CERT_DER, (uint16_t)SSL_CERT_DER_len,
                           (unsigned char*)SSL_KEY_DER,  (uint16_t)SSL_KEY_DER_len);
static HTTPSServer g_https(&g_cert, 443);
static HTTPServer  g_http_redir(80);

// Non-zero = restart at this millis() timestamp (after response is sent).
static uint32_t g_restart_at = 0;

// ── Helper functions ──────────────────────────────────────────────────────────

static String url_decode(const String& s) {
    String r;
    for (int i = 0; i < (int)s.length(); i++) {
        if (s[i] == '+') r += ' ';
        else if (s[i] == '%' && i + 2 < (int)s.length()) {
            char h[3] = {s[i+1], s[i+2], 0};
            r += (char)strtol(h, nullptr, 16);
            i += 2;
        } else r += s[i];
    }
    return r;
}

static String read_body(HTTPRequest* req) {
    String body;
    uint8_t buf[128];
    size_t len;
    while (!req->requestComplete()) {
        len = req->readBytes(buf, sizeof(buf));
        for (size_t i = 0; i < len; i++) body += (char)buf[i];
    }
    return body;
}

// Parse a URL-encoded form body for a specific key.
static String body_param(const String& body, const String& key) {
    String prefix = key + "=";
    int start = 0;
    while (true) {
        int pos = body.indexOf(prefix, start);
        if (pos < 0) return "";
        if (pos == 0 || body[pos - 1] == '&') {
            int vs = pos + prefix.length();
            int end = body.indexOf('&', vs);
            if (end < 0) end = body.length();
            return url_decode(body.substring(vs, end));
        }
        start = pos + 1;
    }
}

static String query_param(HTTPRequest* req, const char* key) {
    std::string val;
    req->getParams()->getQueryParameter(std::string(key), val);
    return String(val.c_str());
}

static void send_json(HTTPResponse* res, int code, const String& json) {
    res->setStatusCode(code);
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Content-Length", String(json.length()).c_str());
    res->print(json.c_str());
}

// ── Cached WiFi scan ──────────────────────────────────────────────────────────

static String   g_scan_cache;
static uint32_t g_scan_ts = 0;

static String get_networks_html() {
    if (g_scan_cache.isEmpty() || millis() - g_scan_ts > 60000UL) {
        int n = WiFi.scanNetworks();
        g_scan_cache = "";
        for (int i = 0; i < n; i++) {
            String s = WiFi.SSID(i);
            g_scan_cache += "<option value=\"" + s + "\">"
                          + s + " (" + WiFi.RSSI(i) + " dBm)</option>";
        }
        if (g_scan_cache.isEmpty())
            g_scan_cache = "<option value=''>No networks found</option>";
        g_scan_ts = millis();
    }
    return g_scan_cache;
}

// ── File Manager HTML ─────────────────────────────────────────────────────────
// Upload uses raw binary body (Content-Type: application/octet-stream) with
// the full destination path as a query parameter: POST /upload?path=/folder/file.txt
// This avoids multipart parsing which is not built into fhessel.

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
</style></head><body>
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
var cp='/',hist=['/'],hIdx=0,g_ent=[],g_sort=0,g_sm=false,g_ent_bk=[],g_st=null;
var SORTS=['&#8645; Name','&#8645; Name&#8595;','&#8645; Size&#8595;'];
function fs(n){return n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':n<1073741824?(n/1048576).toFixed(1)+' MB':(n/1073741824).toFixed(2)+' GB';}
function ft(s){if(s<1)return '<1s';return s<60?Math.ceil(s)+'s':Math.floor(s/60)+'m '+Math.ceil(s%60)+'s';}
function isImg(n){return /\.(jpe?g|png|gif|webp|bmp|svg)$/i.test(n);}
function updNav(){document.getElementById('bk').disabled=hIdx===0;document.getElementById('fw').disabled=hIdx>=hist.length-1;}
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
h+='<button class="act del" onclick="rm(\''+je(fp)+'\')">&#128465;</button></div>';}
});document.getElementById('ls').innerHTML=h||'<div style="color:#555;padding:8px;font-size:.8em">'+(g_sm?'No results':'Empty folder')+'</div>';updSelBar();}
async function loadDir(p){cp=p;setBread(p);document.getElementById('ls').innerHTML='<div style="color:#555;padding:8px;font-size:.8em">Loading&#8230;</div>';
try{var r=await fetch('/api/list?path='+encodeURIComponent(p));var d=await r.json();
if(d.free_gb!=null)document.getElementById('stor').textContent='Free: '+d.free_gb.toFixed(1)+'/'+d.total_gb.toFixed(0)+' GB';
g_ent=d.entries||[];renderDir();}catch(ex){document.getElementById('ls').innerHTML='<div style="color:#f44;padding:8px;font-size:.8em">Load error</div>';}}
function encodeURIComponent2(p){return encodeURIComponent(p).replace(/'/g,'%27');}
function je(s){return s.replace(/\\/g,'\\\\').replace(/'/g,"\\'");}
function ha(s){return s.replace(/&/g,'&amp;').replace(/"/g,'&quot;');}
// Download via fetch+blob so Chrome never applies its "insecure origin" /
// "Check internet connection" block (which fires on link-click downloads from
// self-signed-cert HTTPS pages, especially in AP mode with no real internet).
async function dlBlob(url,name){
  var btn=event&&event.target;if(btn)btn.disabled=true;
  try{
    var r=await fetch(url);
    if(!r.ok){alert('Download error: HTTP '+r.status);return;}
    var blob=await r.blob();
    var burl=URL.createObjectURL(blob);
    var a=document.createElement('a');a.href=burl;a.download=name;
    document.body.appendChild(a);a.click();document.body.removeChild(a);
    setTimeout(function(){URL.revokeObjectURL(burl);},60000);
  }catch(e){alert('Download failed: '+e);}
  finally{if(btn)btn.disabled=false;}
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
var r=await fetch('/api/mkdir',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encodeURIComponent(fp)});
var d=await r.json();if(d.ok)loadDir(cp);}
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
for(var i=0;i<files.length;i++){var file=files[i],destPath;
if(mode===1&&file.webkitRelativePath){var rp=file.webkitRelativePath,ls2=rp.lastIndexOf('/');destPath=(cp==='/'?'':cp)+(ls2>0?'/'+rp.substring(0,ls2):'');}
else{destPath=cp==='/'?'':cp;}var ok=await upXHR(file,destPath,i,files.length,pgEl,spdEl);if(!ok)failed++;}
pgEl.style.display='none';spdEl.textContent='';
if(failed){umEl.textContent=failed+' file(s) failed';umEl.className='msg ng';}
else{umEl.textContent='Done! '+files.length+' file'+(files.length>1?'s':'')+' uploaded';umEl.className='msg ok';}
setTimeout(function(){umEl.className='msg';},4000);loadDir(cp);}
goTo('/');
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
  <label>Network</label>
  <select id="w-ssid" onchange="g_wf=true"><option>Loading...</option></select>
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
async function load(){
  try{
    var r=await fetch('/api/status');d=await r.json();
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
  }catch(e){document.getElementById('fw').textContent='Device offline';}
}
async function loadNets(){
  try{
    var sel=document.getElementById('w-ssid');
    var prev=sel.value;
    var r=await fetch('/api/scan');
    sel.innerHTML=await r.text();
    for(var i=0;i<sel.options.length;i++){if(sel.options[i].value===prev){sel.selectedIndex=i;break;}}
  }catch(e){}
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
load();loadNets();setInterval(load,12000);setInterval(loadNets,30000);
</script>
)html";

// ── HTTP redirect handler (port 80) ──────────────────────────────────────────

static void handle_http_redirect(HTTPRequest* req, HTTPResponse* res) {
    // Windows NCSI probe — answer directly so OS shows "Sign in to network" in AP mode.
    if (wifi_is_ap_mode()) {
        if (req->getRequestString().find("connecttest.txt") != std::string::npos) {
            res->setStatusCode(200);
            res->setHeader("Content-Type", "text/plain");
            res->print("Microsoft Connect Test");
            return;
        }
    }
    String loc = "https://";
    loc += wifi_is_ap_mode() ? AP_IP : wifi_ip();
    loc += "/";
    res->setStatusCode(301);
    res->setHeader("Location", loc.c_str());
    res->print("");
}

// ── HTTPS not-found handler ───────────────────────────────────────────────────

static void handle_not_found(HTTPRequest* req, HTTPResponse* res) {
    if (wifi_is_ap_mode()) {
        res->setStatusCode(302);
        res->setHeader("Location", "https://" AP_IP "/");
        res->print("");
        return;
    }
    res->setStatusCode(404);
    res->setHeader("Content-Type", "text/plain");
    res->print("Not found");
}

// ── Config route handlers ─────────────────────────────────────────────────────

static void handle_root(HTTPRequest* req, HTTPResponse* res) {
    String page = PAGE_HTML;
    page.replace("DEVICE_NAME", "'" DEVICE_NAME "'");
    res->setStatusCode(200);
    res->setHeader("Content-Type", "text/html");
    res->print(page.c_str());
}

static void handle_scan(HTTPRequest* req, HTTPResponse* res) {
    String nets = get_networks_html();
    res->setStatusCode(200);
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Content-Length", String(nets.length()).c_str());
    res->print(nets.c_str());
}

static void handle_status(HTTPRequest* req, HTTPResponse* res) {
    Preferences p;
    p.begin(NVS_NS, true);
    uint8_t mode_val = p.getUChar(NVS_KEY_MODE, (uint8_t)MODE_NETWORK);
    p.end();

    JsonDocument doc;
    doc["fw"]       = FW_VERSION;
    doc["mode"]     = (int)mode_val;
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

    String json;
    serializeJson(doc, json);
    send_json(res, 200, json);
}

static void handle_mode(HTTPRequest* req, HTTPResponse* res) {
    String body = read_body(req);
    int mode = body_param(body, "mode").toInt();
    if (mode != (int)MODE_NETWORK && mode != (int)MODE_USB_DRIVE) {
        send_json(res, 400, "{\"ok\":false,\"error\":\"invalid mode\"}");
        return;
    }
    Preferences p;
    p.begin(NVS_NS, false);
    p.putUChar(NVS_KEY_MODE, (uint8_t)mode);
    p.end();
    lcd_invalidate_layout();
    lcd_splash_msg("Switching...");
    send_json(res, 200, "{\"ok\":true}");
    g_restart_at = millis() + 800;
}

static void handle_wifi(HTTPRequest* req, HTTPResponse* res) {
    String body = read_body(req);
    String ssid = body_param(body, "ssid");
    String pass = body_param(body, "pass");
    if (ssid.isEmpty()) {
        send_json(res, 400, "{\"ok\":false,\"error\":\"ssid required\"}");
        return;
    }
    Preferences p;
    p.begin("wifi", false);
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    uint8_t ip_mode = (uint8_t)body_param(body, "ip_mode").toInt();
    p.putUChar("ip_mode", ip_mode);
    if (ip_mode == 1) {
        String s_mask = body_param(body, "s_mask");
        if (s_mask.isEmpty()) s_mask = "255.255.255.0";
        p.putString("s_ip",   body_param(body, "s_ip"));
        p.putString("s_gw",   body_param(body, "s_gw"));
        p.putString("s_mask", s_mask);
        p.putString("s_dns",  "8.8.8.8");
    }
    p.end();
    send_json(res, 200, "{\"ok\":true}");
    g_restart_at = millis() + 800;
}

static void handle_wifi_reset(HTTPRequest* req, HTTPResponse* res) {
    // Read and discard body (required by fhessel before sending response).
    read_body(req);
    Preferences p;
    p.begin("wifi", false);
    p.clear();
    p.end();
    send_json(res, 200, "{\"ok\":true}");
    g_restart_at = millis() + 800;
}

static void handle_theme(HTTPRequest* req, HTTPResponse* res) {
    String body = read_body(req);
    int id = body_param(body, "id").toInt();
    if (id < 0 || (uint8_t)id >= theme_count()) {
        send_json(res, 400, "{\"ok\":false,\"error\":\"invalid theme\"}");
        return;
    }
    theme_save((uint8_t)id);
    lcd_apply_theme();
    send_json(res, 200, "{\"ok\":true}");
}

// ── File Manager handlers ─────────────────────────────────────────────────────

static void mkdirs(const String& path) {
    for (int i = 1; i < (int)path.length(); i++) {
        if (path[i] == '/') SD_MMC.mkdir(path.substring(0, i));
    }
    if (path.length() > 1) SD_MMC.mkdir(path);
}

static void handle_files_html(HTTPRequest* req, HTTPResponse* res) {
    res->setStatusCode(200);
    res->setHeader("Content-Type", "text/html");
    res->print(FILEMAN_HTML);
}

static void handle_list(HTTPRequest* req, HTTPResponse* res) {
    if (!storage_is_ready()) {
        send_json(res, 503, "{\"error\":\"SD not ready\"}");
        return;
    }
    String path = query_param(req, "path");
    if (path.isEmpty()) path = "/";

    File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) {
        send_json(res, 404, "{\"error\":\"not a directory\"}");
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
        if (name.length() > 0) {
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
    send_json(res, 200, json);
}

// Static 8 KB buffer shared between file download and ZIP streaming.
static uint8_t s_dl_buf[8192];

// SSL_write (via fhessel → mbedTLS) may accept fewer bytes than requested
// on a single call (partial write). Loop until all bytes are sent or the
// connection is closed.
static void res_write_all(HTTPResponse* res, const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t n = res->write(buf + sent, len - sent);
        if (n == 0) break;   // connection closed or error
        sent += n;
    }
}

static void handle_download(HTTPRequest* req, HTTPResponse* res) {
    if (!storage_is_ready()) {
        send_json(res, 503, "{\"error\":\"SD not ready\"}");
        return;
    }
    String path = query_param(req, "path");
    if (path.isEmpty()) {
        res->setStatusCode(400);
        res->setHeader("Content-Type", "text/plain");
        res->print("path required");
        return;
    }
    File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        res->setStatusCode(404);
        res->setHeader("Content-Type", "text/plain");
        res->print("Not found");
        return;
    }
    String name = path.substring(path.lastIndexOf('/') + 1);
    res->setStatusCode(200);
    res->setHeader("Content-Type", "application/octet-stream");
    res->setHeader("Content-Disposition", ("attachment; filename=\"" + name + "\"").c_str());
    res->setHeader("Cache-Control", "no-cache");
    res->setHeader("Content-Length", String(f.size()).c_str());
    while (f.available()) {
        int n = f.read(s_dl_buf, sizeof(s_dl_buf));
        if (n > 0) res_write_all(res, s_dl_buf, (size_t)n);
        yield();
    }
    f.close();
}

// ── ZIP folder download ───────────────────────────────────────────────────────

struct ZipEntry {
    String   zip_name;
    String   sd_path;
    uint32_t size;
    uint32_t crc;
    uint32_t hdr_offset;
};

static std::vector<ZipEntry> s_zip_entries;
static uint32_t              s_zip_offset;
static HTTPResponse*         s_zip_res = nullptr;

static const uint32_t CRC_TAB[16] = {
    0x00000000,0x1db71064,0x3b6e20c8,0x26d930ac,
    0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,
    0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,
    0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c
};
static uint32_t crc32_buf(uint32_t crc, const uint8_t* d, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc = (crc >> 4) ^ CRC_TAB[(crc ^ d[i]) & 0xF];
        crc = (crc >> 4) ^ CRC_TAB[(crc ^ (d[i] >> 4)) & 0xF];
    }
    return ~crc;
}

static void zip_send(const void* d, size_t n) {
    if (s_zip_res) res_write_all(s_zip_res, (const uint8_t*)d, n);
    s_zip_offset += n;
}
static void zip_u16(uint16_t v) { uint8_t b[2]={uint8_t(v),uint8_t(v>>8)}; zip_send(b,2); }
static void zip_u32(uint32_t v) { uint8_t b[4]={uint8_t(v),uint8_t(v>>8),uint8_t(v>>16),uint8_t(v>>24)}; zip_send(b,4); }

static void zip_local_hdr(const String& name, uint32_t crc, uint32_t sz) {
    uint16_t nl = name.length();
    zip_u32(0x04034b50); zip_u16(20); zip_u16(0x0800);  // UTF-8, no data descriptor
    zip_u16(0); zip_u16(0); zip_u16(0);                  // method=stored, mtime=0, mdate=0
    zip_u32(crc); zip_u32(sz); zip_u32(sz);              // CRC, compressed=sz, uncompressed=sz
    zip_u16(nl); zip_u16(0);
    zip_send(name.c_str(), nl);
}
static void zip_data_desc(uint32_t crc, uint32_t sz) {
    zip_u32(0x08074b50); zip_u32(crc); zip_u32(sz); zip_u32(sz);
}
static void zip_central(const ZipEntry& e) {
    uint16_t nl = e.zip_name.length();
    zip_u32(0x02014b50); zip_u16(20); zip_u16(20); zip_u16(0x0800);
    zip_u16(0); zip_u16(0); zip_u16(0);
    zip_u32(e.crc); zip_u32(e.size); zip_u32(e.size);
    zip_u16(nl); zip_u16(0); zip_u16(0); zip_u16(0); zip_u16(0);
    zip_u32(0); zip_u32(e.hdr_offset);
    zip_send(e.zip_name.c_str(), nl);
}

static void zip_collect(const String& sdp, const String& pre) {
    File dir = SD_MMC.open(sdp);
    if (!dir || !dir.isDirectory()) { dir.close(); return; }
    File f = dir.openNextFile();
    while (f) {
        yield();
        String fn = f.name();
        int sl = fn.lastIndexOf('/');
        String base = sl >= 0 ? fn.substring(sl + 1) : fn;
        if (base.length() > 0) {
            String zn = pre.isEmpty() ? base : (pre + "/" + base);
            if (f.isDirectory()) {
                zip_collect(fn, zn);
            } else {
                s_zip_entries.push_back({zn, fn, (uint32_t)f.size(), 0, 0});
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
}

// Pre-read all collected files to compute CRCs before streaming.
// This avoids the data-descriptor (DD) approach where local header CRC=0,
// which causes 7-Zip / WinZip to scan for the PK\x07\x08 signature byte
// sequence inside binary files (e.g. PNG), resulting in CRC errors.
static void zip_precompute_crcs() {
    for (auto& e : s_zip_entries) {
        File f = SD_MMC.open(e.sd_path, FILE_READ);
        uint32_t crc = 0;
        if (f) {
            while (f.available()) {
                int n = f.read(s_dl_buf, sizeof(s_dl_buf));
                if (n > 0) crc = crc32_buf(crc, s_dl_buf, n);
                yield();
            }
            f.close();
        }
        e.crc = crc;
    }
}

static void handle_download_zip(HTTPRequest* req, HTTPResponse* res) {
    if (!storage_is_ready()) {
        res->setStatusCode(503);
        res->setHeader("Content-Type", "text/plain");
        res->print("SD not ready");
        return;
    }
    String path = query_param(req, "path");
    if (path.isEmpty()) path = "/";

    s_zip_entries.clear();
    s_zip_offset = 0;
    String pre;
    if (path != "/") {
        int sl = path.lastIndexOf('/');
        pre = sl >= 0 ? path.substring(sl + 1) : path;
    }
    zip_collect(path, pre);

    if (s_zip_entries.empty()) {
        res->setStatusCode(404);
        res->setHeader("Content-Type", "text/plain");
        res->print("Folder is empty or not found");
        return;
    }

    // Pass 1: compute CRCs so local headers are complete (no data descriptor needed).
    // This avoids 7-Zip/WinZip scanning for PK\x07\x08 inside binary file data.
    zip_precompute_crcs();

    // Content-Length: local headers + file data + central directory + EOCD
    // (No data descriptors since we have CRCs up front.)
    uint32_t total = 22;  // EOCD
    for (const auto& e : s_zip_entries) {
        uint16_t nl = e.zip_name.length();
        total += 30 + nl + e.size;  // local header + file data (no data descriptor)
        total += 46 + nl;           // central directory entry
    }

    String fname = (path == "/") ? "sd_root" : path.substring(path.lastIndexOf('/') + 1);
    fname += ".zip";
    res->setStatusCode(200);
    res->setHeader("Content-Type", "application/zip");
    res->setHeader("Content-Disposition", ("attachment; filename=\"" + fname + "\"").c_str());
    res->setHeader("Cache-Control", "no-cache");
    res->setHeader("Content-Length", String(total).c_str());

    // Pass 2: stream ZIP — local header with correct CRC + size, then file data.
    s_zip_res = res;

    for (auto& e : s_zip_entries) {
        e.hdr_offset = s_zip_offset;
        zip_local_hdr(e.zip_name, e.crc, e.size);
        File f = SD_MMC.open(e.sd_path, FILE_READ);
        if (f) {
            while (f.available()) {
                int n = f.read(s_dl_buf, sizeof(s_dl_buf));
                if (n > 0) zip_send(s_dl_buf, n);
                yield();
            }
            f.close();
        }
        // No data descriptor — CRC and size are already in the local header.
    }

    uint32_t cd_off = s_zip_offset;
    for (const auto& e : s_zip_entries) zip_central(e);
    uint32_t cd_sz = s_zip_offset - cd_off;

    uint16_t cnt = (uint16_t)s_zip_entries.size();
    zip_u32(0x06054b50); zip_u16(0); zip_u16(0);
    zip_u16(cnt); zip_u16(cnt);
    zip_u32(cd_sz); zip_u32(cd_off); zip_u16(0);

    s_zip_res = nullptr;
    s_zip_entries.clear();
}

// Upload: raw binary body, full destination path as query param.
// Client sends: POST /upload?path=/folder/file.txt
//               Content-Type: application/octet-stream
//               <raw file bytes>
static void handle_upload(HTTPRequest* req, HTTPResponse* res) {
    if (!storage_is_ready()) {
        send_json(res, 503, "{\"ok\":false,\"error\":\"SD not ready\"}");
        return;
    }
    String path = query_param(req, "path");
    if (path.isEmpty()) {
        send_json(res, 400, "{\"ok\":false,\"error\":\"path required\"}");
        return;
    }
    int last_slash = path.lastIndexOf('/');
    if (last_slash > 0) mkdirs(path.substring(0, last_slash));

    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
        send_json(res, 500, "{\"ok\":false,\"error\":\"cannot create file\"}");
        return;
    }
    uint8_t buf[512];
    size_t len;
    while (!req->requestComplete()) {
        len = req->readBytes(buf, sizeof(buf));
        if (len > 0) f.write(buf, len);
    }
    f.close();
    send_json(res, 200, "{\"ok\":true}");
}

static void handle_delete(HTTPRequest* req, HTTPResponse* res) {
    String body = read_body(req);
    String path = body_param(body, "path");
    if (path.isEmpty()) {
        send_json(res, 400, "{\"ok\":false}");
        return;
    }
    bool ok = SD_MMC.remove(path) || SD_MMC.rmdir(path);
    send_json(res, 200, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handle_mkdir(HTTPRequest* req, HTTPResponse* res) {
    String body = read_body(req);
    String path = body_param(body, "path");
    if (path.isEmpty()) {
        send_json(res, 400, "{\"ok\":false}");
        return;
    }
    bool ok = SD_MMC.mkdir(path);
    send_json(res, 200, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handle_rename(HTTPRequest* req, HTTPResponse* res) {
    String body = read_body(req);
    String from = body_param(body, "from");
    String to   = body_param(body, "to");
    if (from.isEmpty() || to.isEmpty()) {
        send_json(res, 400, "{\"ok\":false,\"error\":\"params required\"}");
        return;
    }
    bool ok = SD_MMC.rename(from, to);
    send_json(res, 200, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rename failed\"}");
}

// ── Global search ─────────────────────────────────────────────────────────────

static void search_walk(const String& sdp, const String& q, JsonArray arr, int& cnt) {
    if (cnt >= 200) return;
    File dir = SD_MMC.open(sdp);
    if (!dir || !dir.isDirectory()) { dir.close(); return; }
    File f = dir.openNextFile();
    while (f && cnt < 200) {
        yield();
        String fn = f.name();
        int sl = fn.lastIndexOf('/');
        String base = sl >= 0 ? fn.substring(sl + 1) : fn;
        if (base.length() > 0) {
            String bl = base;
            bl.toLowerCase();
            if (bl.indexOf(q) >= 0) {
                JsonObject obj = arr.add<JsonObject>();
                obj["name"]   = base;
                obj["path"]   = fn;
                obj["parent"] = sl > 0 ? fn.substring(0, sl) : String("/");
                obj["dir"]    = f.isDirectory();
                if (!f.isDirectory()) obj["size"] = (uint32_t)f.size();
                cnt++;
            }
            if (f.isDirectory()) search_walk(fn, q, arr, cnt);
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
}

static void handle_search(HTTPRequest* req, HTTPResponse* res) {
    if (!storage_is_ready()) {
        send_json(res, 503, "{\"error\":\"SD not ready\"}");
        return;
    }
    String q = query_param(req, "q");
    q.toLowerCase();
    if (q.isEmpty()) {
        send_json(res, 400, "{\"error\":\"q required\"}");
        return;
    }
    JsonDocument doc;
    JsonArray arr = doc["entries"].to<JsonArray>();
    doc["free_gb"]  = storage_free_gb();
    doc["total_gb"] = storage_total_gb();
    int cnt = 0;
    search_walk("/", q, arr, cnt);
    String json;
    serializeJson(doc, json);
    send_json(res, 200, json);
}

// ── Public API ────────────────────────────────────────────────────────────────

void web_server_begin() {
    // ── HTTPS server (port 443) — all routes ─────────────────────────────────
    g_https.registerNode(new ResourceNode("/",               "GET",  handle_root));
    g_https.registerNode(new ResourceNode("/api/status",     "GET",  handle_status));
    g_https.registerNode(new ResourceNode("/api/scan",       "GET",  handle_scan));
    g_https.registerNode(new ResourceNode("/api/mode",       "POST", handle_mode));
    g_https.registerNode(new ResourceNode("/api/wifi",       "POST", handle_wifi));
    g_https.registerNode(new ResourceNode("/api/wifi-reset", "POST", handle_wifi_reset));
    g_https.registerNode(new ResourceNode("/api/theme",      "POST", handle_theme));
    g_https.registerNode(new ResourceNode("/files",          "GET",  handle_files_html));
    g_https.registerNode(new ResourceNode("/api/list",       "GET",  handle_list));
    g_https.registerNode(new ResourceNode("/api/search",     "GET",  handle_search));
    g_https.registerNode(new ResourceNode("/download",       "GET",  handle_download));
    g_https.registerNode(new ResourceNode("/download-zip",   "GET",  handle_download_zip));
    g_https.registerNode(new ResourceNode("/upload",         "POST", handle_upload));
    g_https.registerNode(new ResourceNode("/api/delete",     "POST", handle_delete));
    g_https.registerNode(new ResourceNode("/api/mkdir",      "POST", handle_mkdir));
    g_https.registerNode(new ResourceNode("/api/rename",     "POST", handle_rename));
    g_https.setDefaultNode(new ResourceNode("", "GET", handle_not_found));
    g_https.start();

    // ── HTTP server (port 80) — redirect everything to HTTPS ─────────────────
    g_http_redir.setDefaultNode(new ResourceNode("", "GET", handle_http_redirect));
    g_http_redir.start();

    Serial.println("[HTTPS] Server started on port 443");
    Serial.println("[HTTP]  Redirect server started on port 80");
}

void web_server_loop() {
    g_https.loop();
    g_http_redir.loop();
    // Deferred restart: lets the handler return and the response be fully sent
    // before the device restarts. Set g_restart_at = millis() + delay in handlers.
    if (g_restart_at > 0 && millis() >= g_restart_at) {
        ESP.restart();
    }
}
