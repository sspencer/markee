#include "webui.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Split into <4095-byte ISO C11 string literals to stay clean under -Wpedantic. */
static const char *const WEBUI_PARTS[] = {
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>markee Studio</title>"
    "<style>"
    "*{box-sizing:border-box}body{margin:0;background:#16181d;color:#e6e8ec;font:14px/1.45 -apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif;padding:20px}"
    ".wrap{max-width:920px;margin:0 auto}h1{margin:0 0 6px;font-size:20px;display:flex;align-items:center;gap:10px}"
    ".meta{color:#8f96a3;font-size:12px;margin-bottom:16px}"
    ".bezel{background:#e6e3dc;padding:16px;border-radius:18px;box-shadow:0 12px 32px rgba(0,0,0,.45);margin-bottom:18px}"
    ".face{background:#101216;padding:12px;border-radius:10px}"
    "canvas{width:100%;height:auto;display:block;image-rendering:pixelated;border-radius:4px;cursor:crosshair}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}@media(max-width:720px){.grid{grid-template-columns:1fr}}"
    ".card{background:#1f232b;border:1px solid #2d333f;border-radius:10px;padding:14px}"
    ".card h2{margin:0 0 10px;font-size:14px;color:#b8c0ce;text-transform:uppercase;letter-spacing:.05em}"
    "textarea{width:100%;min-height:130px;background:#13161b;color:#d6e4ff;border:1px solid #303846;border-radius:6px;padding:8px;font:12px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace}"
    "button,input,select{background:#2a303c;color:#e6e8ec;border:1px solid #3a4252;border-radius:6px;padding:6px 10px;font:13px inherit}"
    "button{cursor:pointer}button:hover{background:#363e4e}button.pri{background:#e26d24;border-color:#e26d24;color:#fff}"
    ".row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:8px}"
    ".drop{border:2px dashed #3b4456;border-radius:8px;padding:18px;text-align:center;color:#9aa3b2;cursor:pointer;margin-bottom:8px}"
    ".drop.over{border-color:#e26d24;color:#fff;background:#262b36}"
    ".status{margin-top:8px;font-size:12px;color:#8ad8a0;min-height:18px}.err{color:#ff8484}"
    "</style></head><body><div class='wrap'>"
    "<h1>markee Studio <span id='res' class='meta'></span></h1>"
    "<div class='meta' id='info'>Connecting...</div>"
    "<div class='bezel'><div class='face'><canvas id='cv'></canvas></div></div>",

    "<div class='grid'>"
    "<div class='card'><h2>Draw JSON (POST /draw)</h2>"
    "<textarea id='drawJson'>[\n"
    "  {\"op\":\"clear\",\"color\":\"#001020\"},\n"
    "  {\"op\":\"icon\",\"name\":\"check\",\"x\":3,\"y\":4,\"color\":\"green\"},\n"
    "  {\"op\":\"text\",\"x\":15,\"y\":4,\"text\":\"READY\",\"color\":\"warmwhite\"}\n"
    "]</textarea>"
    "<div class='row'>"
    "<button class='pri' onclick='runDraw()'>Send /draw</button>"
    "<button onclick='presetClock()'>Clock</button>"
    "<button onclick='presetTimer()'>25m Timer</button>"
    "<button onclick='presetGrad()'>Gradient</button>"
    "<button onclick='post(\"/clear\")'>Clear</button>"
    "<button onclick='toggleDemo()'>Demo</button>"
    "</div><div id='drawStatus' class='status'></div></div>"
    "<div class='card'><h2>Upload GIF / PNG</h2>"
    "<div id='drop' class='drop'>Drop a .gif or .png here, or click to pick</div>"
    "<input id='fileIn' type='file' accept='.gif,.png,image/gif,image/png' hidden>"
    "<div class='row'>"
    "<label>Fill <input id='gifFill' value='black' style='width:80px'></label>"
    "<label>Fit <select id='gifFit'><option>crop</option><option>contain</option><option>stretch</option></select></label>"
    "<label>Speed <input id='gifSpd' type='number' step='0.25' min='0.1' max='10' value='1' style='width:64px'></label>"
    "</div><div class='row'>"
    "<button onclick='post(\"/animate/pause\")'>Pause</button>"
    "<button onclick='post(\"/animate/resume\")'>Resume</button>"
    "<button onclick='req(\"DELETE\",\"/animate\")'>Stop GIF</button>"
    "</div><div id='upStatus' class='status'></div></div>"
    "<div class='card'><h2>Display Controls</h2>"
    "<div class='row'><label>Brightness <input id='bri' type='range' min='0' max='100' value='100' oninput='setBri(this.value)'></label><span id='briVal'>100%</span></div>"
    "<div class='row'><span>Monochrome:</span>"
    "<button onclick='setMono(\"off\")'>Off</button>"
    "<button onclick='setMono(\"amber\")'>Amber</button>"
    "<button onclick='setMono(\"#00ff66\")'>Green</button>"
    "<button onclick='setMono(\"#ff3030\")'>Red</button>"
    "<button onclick='setMono(\"cyan\")'>Cyan</button>"
    "</div></div>"
    "<div class='card'><h2>Alert Sounds & Notify</h2>"
    "<div class='row'>"
    "<button onclick='post(\"/alert/ding\")'>Ding</button>"
    "<button onclick='post(\"/alert/chime\")'>Chime</button>"
    "<button onclick='post(\"/alert/bell\")'>Bell</button>"
    "</div><div class='row'>"
    "<input id='notifTxt' placeholder='Notification text...' value='BUILD OK!' style='flex:1'>"
    "<button class='pri' onclick='sendNotify()'>Notify (4s)</button>"
    "</div></div></div></div>",

    "<script>"
    "let W=72,H=16,demo=false;"
    "const cv=document.getElementById('cv'),ctx=cv.getContext('2d');"
    "const TK=new URLSearchParams(location.search).get('token');"
    "function auth(h){h=h||{};if(TK)h['Authorization']='Bearer '+TK;return h;}"
    "async function req(m,p,b,ct){const h=auth();if(ct)h['Content-Type']=ct;"
    "const r=await fetch(p,{method:m,body:b,headers:h});return{ok:r.ok,status:r.status,text:await r.text()};}"
    "async function post(p){await req('POST',p);pollInfo();}"
    "async function pollInfo(){try{const r=await fetch('/info',{headers:auth()});const j=await r.json();"
    "W=j.width;H=j.height;demo=j.demo_active;"
    "document.getElementById('res').textContent=W+'x'+H;"
    "document.getElementById('info').textContent=`Frames: ${j.frames_received} | Brightness: ${j.brightness}% | Mono: ${j.monochrome||'off'} | Anim: ${j.animation_active?'on':'off'}`;"
    "document.getElementById('bri').value=j.brightness;document.getElementById('briVal').textContent=j.brightness+'%';"
    "}catch(e){}}"
    "async function pollFrame(){try{const r=await fetch('/frame?format=raw',{headers:auth()});"
    "const buf=new Uint8Array(await r.arrayBuffer());const s=10;if(cv.width!==W*s||cv.height!==H*s){cv.width=W*s;cv.height=H*s;}"
    "ctx.fillStyle='#0a0b0e';ctx.fillRect(0,0,cv.width,cv.height);"
    "for(let y=0;y<H;y++)for(let x=0;x<W;x++){const i=(y*W+x)*3;"
    "const cr=buf[i],cg=buf[i+1],cb=buf[i+2];"
    "ctx.fillStyle=(cr||cg||cb)?`rgb(${cr},${cg},${cb})`:'#181a20';"
    "ctx.fillRect(x*s+1,y*s+1,s-2,s-2);}}catch(e){}}"
    "cv.addEventListener('click',e=>{const rc=cv.getBoundingClientRect();"
    "const x=Math.floor((e.clientX-rc.left)/rc.width*W),y=Math.floor((e.clientY-rc.top)/rc.height*H);"
    "req('POST','/events',JSON.stringify({type:'click',x,y,detail:'left'}),'application/json');});"
    "async function runDraw(){const st=document.getElementById('drawStatus');"
    "const r=await req('POST','/draw',document.getElementById('drawJson').value,'application/json');"
    "st.className=r.ok?'status':'status err';st.textContent=r.text;pollInfo();}"
    "function presetClock(){document.getElementById('drawJson').value=JSON.stringify([{op:'clear'},{op:'clock',color:'warmwhite'}],null,2);runDraw();}"
    "function presetTimer(){document.getElementById('drawJson').value=JSON.stringify([{op:'clear'},{op:'countdown',seconds:1500,color:'amber',bar:true,sound:'chime'}],null,2);runDraw();}"
    "function presetGrad(){document.getElementById('drawJson').value=JSON.stringify([{op:'gradient',from:'#001840',to:'#500028'},{op:'text',y:4,text:'MARKEE',align:'center',color:'white'}],null,2);runDraw();}"
    "function toggleDemo(){post('/demo?enabled='+(demo?0:1));}"
    "function setBri(v){document.getElementById('briVal').textContent=v+'%';post('/brightness?value='+v);}"
    "function setMono(c){post('/monochrome?color='+encodeURIComponent(c));}"
    "function sendNotify(){const t=document.getElementById('notifTxt').value;"
    "req('POST','/notify',JSON.stringify({text:t,color:'warmwhite',bg:'#001a0a',border:'green',ttl:4,sound:'chime'}),'application/json');}"
    "const drop=document.getElementById('drop'),fi=document.getElementById('fileIn');"
    "drop.onclick=()=>fi.click();"
    "drop.ondragover=e=>{e.preventDefault();drop.classList.add('over');};"
    "drop.ondragleave=()=>drop.classList.remove('over');"
    "drop.ondrop=e=>{e.preventDefault();drop.classList.remove('over');if(e.dataTransfer.files[0])uploadFile(e.dataTransfer.files[0]);};"
    "fi.onchange=()=>{if(fi.files[0])uploadFile(fi.files[0]);};"
    "async function uploadFile(f){const st=document.getElementById('upStatus');const buf=await f.arrayBuffer();"
    "const isGif=f.name.toLowerCase().endsWith('.gif')||f.type==='image/gif';"
    "const fill=encodeURIComponent(document.getElementById('gifFill').value||'black');"
    "const fit=encodeURIComponent(document.getElementById('gifFit').value);"
    "const spd=encodeURIComponent(document.getElementById('gifSpd').value||'1');"
    "const url=isGif?`/animate?fill=${fill}&fit=${fit}&speed=${spd}`:'/frame';"
    "const r=await req('POST',url,buf,isGif?'image/gif':'image/png');"
    "st.className=r.ok?'status':'status err';st.textContent=r.text;pollInfo();}"
    "pollInfo();pollFrame();setInterval(pollFrame,100);setInterval(pollInfo,1500);"
    "</script></body></html>"
};

static char *g_html = NULL;
static size_t g_html_len = 0;
static pthread_once_t g_html_once = PTHREAD_ONCE_INIT;

static void build_webui_html(void) {
    size_t n = sizeof(WEBUI_PARTS) / sizeof(WEBUI_PARTS[0]);
    size_t total = 0;
    for (size_t i = 0; i < n; i++) total += strlen(WEBUI_PARTS[i]);
    g_html = malloc(total + 1);
    if (!g_html) return;
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(WEBUI_PARTS[i]);
        memcpy(g_html + off, WEBUI_PARTS[i], len);
        off += len;
    }
    g_html[total] = '\0';
    g_html_len = total;
}

const char *webui_html(size_t *out_len) {
    pthread_once(&g_html_once, build_webui_html);
    if (out_len) *out_len = g_html_len;
    return g_html ? g_html : "";
}
