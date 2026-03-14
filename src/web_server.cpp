#include "web_server.h"
#include "config.h"
#include "wifi_manager.h"
#include "storage.h"
#include "themes.h"
#include "lcd.h"
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <vector>

static WebServer g_http(80);

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
function dlBlob(url,name){var a=document.createElement('a');a.href=url;a.download=name;document.body.appendChild(a);a.click();document.body.removeChild(a);}
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
var fd=new FormData();fd.append('file',file);xhr.open('POST','/upload?path='+encodeURIComponent(path));xhr.send(fd);});}
async function doUp(mode){var fiEl=document.getElementById('fi'+mode),files=fiEl.files;if(!files.length)return;
var pgEl=document.getElementById('pg'+mode),spdEl=document.getElementById('spd'+mode),umEl=document.getElementById('um'+mode);
pgEl.style.display='block';spdEl.textContent='Starting\u2026';var failed=0;
for(var i=0;i<files.length;i++){var file=files[i],destPath;
if(mode===1&&file.webkitRelativePath){var rp=file.webkitRelativePath,ls2=rp.lastIndexOf('/');destPath=(cp==='/'?'':cp)+(ls2>0?'/'+rp.substring(0,ls2):'');}
else{destPath=cp;}var ok=await upXHR(file,destPath,i,files.length,pgEl,spdEl);if(!ok)failed++;}
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

// ── Route handlers ────────────────────────────────────────────────────────────

static void handle_root() {
    // Inline DEVICE_NAME so the JS variable resolves without template substitution.
    String page = PAGE_HTML;
    page.replace("DEVICE_NAME", "'" DEVICE_NAME "'");
    g_http.send(200, "text/html", page);
}

static void handle_scan() {
    g_http.send(200, "text/html", get_networks_html());
}

static void handle_status() {
    // Read current mode from NVS (source of truth).
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

    // Static IP config (stored in "wifi" NVS namespace alongside credentials)
    Preferences wP;
    wP.begin("wifi", true);
    doc["ip_mode"] = (int)wP.getUChar("ip_mode", 0);
    doc["s_ip"]    = wP.getString("s_ip",   "");
    doc["s_gw"]    = wP.getString("s_gw",   "");
    doc["s_mask"]  = wP.getString("s_mask",  "255.255.255.0");
    wP.end();

    String json;
    serializeJson(doc, json);
    g_http.send(200, "application/json", json);
}

static void handle_mode() {
    int mode = g_http.arg("mode").toInt();
    if (mode != (int)MODE_NETWORK && mode != (int)MODE_USB_DRIVE) {
        g_http.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid mode\"}");
        return;
    }
    Preferences p;
    p.begin(NVS_NS, false);
    p.putUChar(NVS_KEY_MODE, (uint8_t)mode);
    p.end();
    lcd_invalidate_layout();
    lcd_splash_msg("Switching...");
    g_http.send(200, "application/json", "{\"ok\":true}");
    delay(600);
    ESP.restart();
}

static void handle_wifi() {
    String ssid = g_http.arg("ssid");
    String pass = g_http.arg("pass");
    if (ssid.isEmpty()) {
        g_http.send(400, "application/json", "{\"ok\":false,\"error\":\"ssid required\"}");
        return;
    }
    Preferences p;
    p.begin("wifi", false);
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    uint8_t ip_mode = (uint8_t)g_http.arg("ip_mode").toInt();
    p.putUChar("ip_mode", ip_mode);
    if (ip_mode == 1) {
        String s_mask = g_http.arg("s_mask"); if (s_mask.isEmpty()) s_mask = "255.255.255.0";
        p.putString("s_ip",   g_http.arg("s_ip"));
        p.putString("s_gw",   g_http.arg("s_gw"));
        p.putString("s_mask", s_mask);
        p.putString("s_dns",  "8.8.8.8");
    }
    p.end();
    g_http.send(200, "application/json", "{\"ok\":true}");
    delay(500);
    ESP.restart();
}

static void handle_wifi_reset() {
    g_http.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    wifi_reset_credentials();   // clears NVS and restarts
}

static void handle_theme() {
    int id = g_http.arg("id").toInt();
    if (id < 0 || (uint8_t)id >= theme_count()) {
        g_http.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid theme\"}");
        return;
    }
    theme_save((uint8_t)id);
    lcd_apply_theme();   // live preview on LCD
    g_http.send(200, "application/json", "{\"ok\":true}");
}

// ── File Manager handlers ─────────────────────────────────────────────────────

// Recursively create all directories in path (SD_MMC.mkdir is not recursive).
static void mkdirs(const String& path) {
    for (int i = 1; i < (int)path.length(); i++) {
        if (path[i] == '/') SD_MMC.mkdir(path.substring(0, i));
    }
    if (path.length() > 1) SD_MMC.mkdir(path);
}

static void handle_files_html() {
    g_http.send(200, "text/html", FILEMAN_HTML);
}

static void handle_list() {
    if (!storage_is_ready()) {
        g_http.send(503, "application/json", "{\"error\":\"SD not ready\"}");
        return;
    }
    String path = g_http.hasArg("path") ? g_http.arg("path") : "/";
    if (path.isEmpty()) path = "/";

    File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) {
        g_http.send(404, "application/json", "{\"error\":\"not a directory\"}");
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
        // SD_MMC returns full path from root; extract filename only.
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
    g_http.send(200, "application/json", json);
}

// Static 8 KB buffer: avoids a large stack frame in the handler.
static uint8_t s_dl_buf[8192];

static void handle_download() {
    if (!storage_is_ready()) {
        g_http.send(503, "text/plain", "SD not ready");
        return;
    }
    String path = g_http.arg("path");
    if (path.isEmpty()) {
        g_http.send(400, "text/plain", "path required");
        return;
    }
    File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        g_http.send(404, "text/plain", "Not found");
        return;
    }
    String name = path.substring(path.lastIndexOf('/') + 1);
    g_http.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    g_http.sendHeader("Cache-Control", "no-cache");
    g_http.setContentLength(f.size());
    g_http.send(200, "application/octet-stream", "");
    while (f.available()) {
        int n = f.read(s_dl_buf, sizeof(s_dl_buf));
        if (n > 0) g_http.sendContent((const char*)s_dl_buf, n);
        yield();
    }
    f.close();
}

// ── ZIP folder download ───────────────────────────────────────────────────────
// Streams a stored (no-compression) ZIP of an entire SD directory.
// CRC-32 is computed on-the-fly using a data descriptor after each file entry,
// so the directory is only traversed once.  Content-Length is pre-calculated
// from file sizes so the browser can show a progress bar.

struct ZipEntry {
    String   zip_name;   // relative path inside the ZIP
    String   sd_path;    // absolute path on SD card
    uint32_t size;
    uint32_t crc;
    uint32_t hdr_offset; // byte offset of local file header in the stream
};

static std::vector<ZipEntry> s_zip_entries;
static uint32_t              s_zip_offset;

// Nibble-at-a-time CRC-32 (half-table, saves 48 bytes vs full table).
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

static void zip_send(const void* d, size_t n) { g_http.sendContent((const char*)d, n); s_zip_offset += n; }
static void zip_u16(uint16_t v) { uint8_t b[2]={uint8_t(v),uint8_t(v>>8)}; zip_send(b,2); }
static void zip_u32(uint32_t v) { uint8_t b[4]={uint8_t(v),uint8_t(v>>8),uint8_t(v>>16),uint8_t(v>>24)}; zip_send(b,4); }

static void zip_local_hdr(const String& name, uint32_t sz, bool dd) {
    uint16_t nl = name.length();
    // flag 0x0808 = bit3 (data descriptor follows) + bit11 (UTF-8 filename)
    zip_u32(0x04034b50); zip_u16(20); zip_u16(dd ? 0x0808 : 0x0800);
    zip_u16(0); zip_u16(0); zip_u16(0);              // compression, mod-time, mod-date
    zip_u32(0); zip_u32(dd ? 0 : sz); zip_u32(dd ? 0 : sz); // crc=0 (filled by DD), sizes
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

static void handle_download_zip() {
    if (!storage_is_ready()) {
        g_http.send(503, "text/plain", "SD not ready");
        return;
    }
    String path = g_http.arg("path");
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
        g_http.send(404, "text/plain", "Folder is empty or not found");
        return;
    }

    // Pre-calculate exact ZIP byte length so the browser can show download progress.
    // Per file: 30+nl (local hdr) + size (data) + 16 (data descriptor) + 46+nl (central dir)
    uint32_t total = 22; // end-of-central-directory record
    for (const auto& e : s_zip_entries) {
        uint16_t nl = e.zip_name.length();
        total += 30 + nl + e.size + 16;  // local header + data + data descriptor
        total += 46 + nl;                // central directory entry
    }

    String fname = (path == "/") ? "sd_root" : path.substring(path.lastIndexOf('/') + 1);
    fname += ".zip";
    g_http.sendHeader("Content-Disposition", "attachment; filename=\"" + fname + "\"");
    g_http.sendHeader("Cache-Control", "no-cache");
    g_http.setContentLength(total);
    g_http.send(200, "application/zip", "");

    // Stream local file headers + raw file data
    for (auto& e : s_zip_entries) {
        e.hdr_offset = s_zip_offset;
        zip_local_hdr(e.zip_name, e.size, true);
        File f = SD_MMC.open(e.sd_path, FILE_READ);
        uint32_t crc = 0;
        if (f) {
            while (f.available()) {
                int n = f.read(s_dl_buf, sizeof(s_dl_buf));
                if (n > 0) { crc = crc32_buf(crc, s_dl_buf, n); zip_send(s_dl_buf, n); }
                yield();
            }
            f.close();
        }
        e.crc = crc;
        zip_data_desc(crc, e.size);
    }

    // Central directory
    uint32_t cd_off = s_zip_offset;
    for (const auto& e : s_zip_entries) zip_central(e);
    uint32_t cd_sz = s_zip_offset - cd_off;

    // End of central directory record
    uint16_t cnt = (uint16_t)s_zip_entries.size();
    zip_u32(0x06054b50); zip_u16(0); zip_u16(0);
    zip_u16(cnt); zip_u16(cnt);
    zip_u32(cd_sz); zip_u32(cd_off); zip_u16(0);

    s_zip_entries.clear();
}

static File   s_upload_file;
static String s_upload_dest;

static void handle_upload_chunk() {
    HTTPUpload& up = g_http.upload();
    if (up.status == UPLOAD_FILE_START) {
        String folder = g_http.hasArg("path") ? g_http.arg("path") : "/";
        if (!folder.endsWith("/")) folder += "/";
        s_upload_dest = folder + up.filename;
        // Ensure all parent directories exist (required for folder uploads).
        int last_slash = s_upload_dest.lastIndexOf('/');
        if (last_slash > 0) mkdirs(s_upload_dest.substring(0, last_slash));
        s_upload_file = SD_MMC.open(s_upload_dest, FILE_WRITE);
        Serial.printf("[HTTP] Upload start: %s\n", s_upload_dest.c_str());
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (s_upload_file) s_upload_file.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
        if (s_upload_file) { s_upload_file.close(); }
        Serial.printf("[HTTP] Upload done: %s (%u bytes)\n",
                      s_upload_dest.c_str(), up.totalSize);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        if (s_upload_file) { s_upload_file.close(); }
        SD_MMC.remove(s_upload_dest);
    }
}

static void handle_upload_done() {
    g_http.send(200, "application/json", "{\"ok\":true}");
}

static void handle_delete() {
    String path = g_http.arg("path");
    if (path.isEmpty()) {
        g_http.send(400, "application/json", "{\"ok\":false}");
        return;
    }
    bool ok = SD_MMC.remove(path) || SD_MMC.rmdir(path);
    g_http.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handle_mkdir() {
    String path = g_http.arg("path");
    if (path.isEmpty()) {
        g_http.send(400, "application/json", "{\"ok\":false}");
        return;
    }
    bool ok = SD_MMC.mkdir(path);
    g_http.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handle_rename() {
    String from = g_http.arg("from");
    String to   = g_http.arg("to");
    if (from.isEmpty() || to.isEmpty()) {
        g_http.send(400, "application/json", "{\"ok\":false,\"error\":\"params required\"}");
        return;
    }
    bool ok = SD_MMC.rename(from, to);
    g_http.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rename failed\"}");
}

// ── Global search ─────────────────────────────────────────────────────────────
// Recursively walks the SD card and collects entries whose name contains q.
// Results are capped at 200 to limit JSON size.

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
            if (f.isDirectory()) {
                search_walk(fn, q, arr, cnt);
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
}

static void handle_search() {
    if (!storage_is_ready()) {
        g_http.send(503, "application/json", "{\"error\":\"SD not ready\"}");
        return;
    }
    String q = g_http.arg("q");
    q.toLowerCase();
    if (q.isEmpty()) {
        g_http.send(400, "application/json", "{\"error\":\"q required\"}");
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
    g_http.send(200, "application/json", json);
}

static void handle_not_found() {
    if (!wifi_is_ap_mode()) {
        g_http.send(404, "text/plain", "Not found");
        return;
    }
    // Windows captive-portal detection: expects "Microsoft Connect Test" from
    // /connecttest.txt — returning it tells Windows the portal is active and
    // triggers the "Sign in to network" notification correctly.
    String uri = g_http.uri();
    if (uri == "/connecttest.txt") {
        g_http.send(200, "text/plain", "Microsoft Connect Test");
        return;
    }
    // All other unknown paths → redirect to config dashboard.
    g_http.sendHeader("Location", "http://" AP_IP "/", true);
    g_http.send(302, "text/plain", "");
}

// ── Public API ────────────────────────────────────────────────────────────────

void web_server_begin() {
    g_http.on("/",             HTTP_GET,  handle_root);
    g_http.on("/api/status",   HTTP_GET,  handle_status);
    g_http.on("/api/scan",     HTTP_GET,  handle_scan);
    g_http.on("/api/mode",     HTTP_POST, handle_mode);
    g_http.on("/api/wifi",     HTTP_POST, handle_wifi);
    g_http.on("/api/wifi-reset", HTTP_POST, handle_wifi_reset);
    g_http.on("/api/theme",    HTTP_POST, handle_theme);
    // ── File Manager ──────────────────────────────────────────────────────────
    g_http.on("/files",      HTTP_GET,  handle_files_html);
    g_http.on("/api/list",   HTTP_GET,  handle_list);
    g_http.on("/api/search", HTTP_GET,  handle_search);
    g_http.on("/download",     HTTP_GET,  handle_download);
    g_http.on("/download-zip", HTTP_GET,  handle_download_zip);
    g_http.on("/upload",     HTTP_POST, handle_upload_done, handle_upload_chunk);
    g_http.on("/api/delete", HTTP_POST, handle_delete);
    g_http.on("/api/mkdir",  HTTP_POST, handle_mkdir);
    g_http.on("/api/rename", HTTP_POST, handle_rename);
    g_http.onNotFound(handle_not_found);
    g_http.begin();
    Serial.println("[HTTP] Config server started on port 80");
}

void web_server_loop() {
    g_http.handleClient();
}
