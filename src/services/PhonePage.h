#pragma once

namespace eflow {

// The page a phone opens from the QR code (served by PhoneShare, never from the internet): it waits for the PC to allow
// it, then lists the files offered on the PC (to download) and sends files to the PC. Plain HTML, CSS and JavaScript in
// one string, with no external scripts, fonts or images. Text from the PC (file names) is only ever set as textContent.
inline constexpr const char* kPhonePage = R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="light dark">
<title>Eflow</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 512 512'%3E%3Crect width='512' height='512' rx='116' fill='%235865F2'/%3E%3Cg fill='%23fff' stroke='%23fff' stroke-width='36' stroke-linejoin='round'%3E%3Cpath d='M138 250 L166 130 L230 214 Z'/%3E%3Cpath d='M374 250 L346 130 L282 214 Z'/%3E%3C/g%3E%3Crect x='92' y='196' width='328' height='184' rx='92' fill='%23fff'/%3E%3Cg fill='none' stroke='%233A42D8' stroke-width='22' stroke-linecap='round'%3E%3Cpath d='M176 300 Q204 266 232 300'/%3E%3Cpath d='M280 300 Q308 266 336 300'/%3E%3C/g%3E%3C/svg%3E">
<style>
:root{--bg:#f4f5f7;--card:#fff;--text:#16171a;--dim:#6b6f78;--line:#e3e5ea;--accent:#0a7cff;--good:#17a34a;--bad:#d93a2f}
@media (prefers-color-scheme:dark){:root{--bg:#0d0e10;--card:#17181b;--text:#f2f3f5;--dim:#9a9ea8;--line:#26282d;--accent:#4c9dff;--good:#3ccf74;--bad:#ff6b5f}}
*{box-sizing:border-box}html,body{margin:0;background:var(--bg);color:var(--text);font:16px/1.4 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
main{max-width:560px;margin:0 auto;padding:max(20px,env(safe-area-inset-top)) 16px 40px}
header{display:flex;align-items:center;gap:10px;margin:4px 0 18px}
.logo{width:34px;height:34px;display:block;flex:none}
h1{font-size:19px;margin:0}header p{margin:0;color:var(--dim);font-size:13px}
.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:16px;margin-bottom:14px}
h2{font-size:15px;margin:0 0 10px}.dim{color:var(--dim);font-size:14px}
.row{display:flex;align-items:center;gap:12px;padding:10px 0;border-top:1px solid var(--line)}.row:first-of-type{border-top:0}
.name{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.size{color:var(--dim);font-size:13px}
a.btn,button{appearance:none;border:0;border-radius:12px;background:var(--accent);color:#fff;font:600 15px/1 inherit;padding:11px 16px;text-decoration:none;cursor:pointer}
button.big{width:100%;padding:16px;font-size:16px}
.bar{height:6px;border-radius:3px;background:var(--line);overflow:hidden;margin-top:6px}.bar i{display:block;height:100%;width:0;background:var(--accent)}
.ok{color:var(--good)}.err{color:var(--bad)}.center{text-align:center;padding:30px 16px}
.spin{width:28px;height:28px;border:3px solid var(--line);border-top-color:var(--accent);border-radius:50%;margin:0 auto 14px;animation:s 1s linear infinite}
@keyframes s{to{transform:rotate(360deg)}}[hidden]{display:none!important}
</style></head>
<body><main>
<header><svg class="logo" viewBox="0 0 512 512" aria-hidden="true"><defs><linearGradient id="tile" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#7C86FF"/><stop offset="1" stop-color="#3A42D8"/></linearGradient></defs><rect width="512" height="512" rx="116" fill="url(#tile)"/><g fill="#fff" stroke="#fff" stroke-width="36" stroke-linejoin="round"><path d="M138 250 L166 130 L230 214 Z"/><path d="M374 250 L346 130 L282 214 Z"/></g><rect x="92" y="196" width="328" height="184" rx="92" fill="#fff"/><g fill="none" stroke="#3A42D8" stroke-width="22" stroke-linecap="round"><path d="M176 300 Q204 266 232 300"/><path d="M280 300 Q308 266 336 300"/></g></svg><div><h1>Eflow</h1><p id="pc">Your PC</p></div></header>
<section id="wait" class="card center"><div class="spin"></div><h2>Waiting for your PC</h2>
<p class="dim">Choose <b>Allow</b> on the PC's island to connect this phone.</p></section>
<section id="stop" class="card center" hidden><h2 id="stopTitle">Not connected</h2><p class="dim" id="stopText"></p></section>
<div id="on" hidden>
<section class="card"><h2>From your PC</h2><div id="files"></div>
<p class="dim" id="none">Nothing yet. Drop files on the SafeShare tab on your PC and they appear here.</p></section>
<section class="card"><h2>Send to your PC</h2>
<p class="dim">They land in the PC's Downloads, in "From phone", and on the island's Shelf.</p>
<input id="pick" type="file" multiple hidden><button class="big" id="choose">Choose files</button><div id="sent"></div></section>
</div>
</main>
<script>
"use strict";
const $=id=>document.getElementById(id);
const size=n=>n<1024?n+" B":n<1048576?(n/1024).toFixed(0)+" KB":n<1073741824?(n/1048576).toFixed(1)+" MB":(n/1073741824).toFixed(2)+" GB";
let shown="",busy=false,timer=0;
function show(which,title,text){$("wait").hidden=which!=="wait";$("on").hidden=which!=="on";$("stop").hidden=which!=="stop";
 if(which==="stop"){$("stopTitle").textContent=title;$("stopText").textContent=text;}}
function render(files){const key=JSON.stringify(files);if(key===shown)return;shown=key;const list=$("files");list.replaceChildren();
 $("none").hidden=files.length>0;
 for(const f of files){const row=document.createElement("div");row.className="row";const n=document.createElement("div");n.className="name";
  n.textContent=f.name;const s=document.createElement("span");s.className="size";s.textContent=size(f.size);const a=document.createElement("a");
  a.className="btn";a.textContent="Download";a.href="api/file/"+encodeURIComponent(f.id);a.setAttribute("download","");row.append(n,s,a);list.append(row);}}
async function poll(){try{const r=await fetch("api/state",{cache:"no-store"});const s=await r.json();if(s.pc)$("pc").textContent=s.pc;
  if(s.status==="allowed"){show("on");render(s.files||[]);}
  else if(s.status==="waiting")show("wait");
  else{show("stop",s.status==="denied"?"Not allowed":s.status==="busy"?"Another phone is connected":"Disconnected",
   s.status==="denied"?"The PC didn't allow this phone.":s.status==="busy"?"Only one phone can connect at a time.":"Connect again from Eflow on your PC.");
   clearInterval(timer);return;}}
 catch(e){show("stop","Can't reach your PC","Make sure the phone is on the same Wi-Fi and Eflow is still connected.");}}
function upload(file){return new Promise(done=>{const row=document.createElement("div");row.className="row";const box=document.createElement("div");
 box.className="name";const n=document.createElement("div");n.textContent=file.name;const bar=document.createElement("div");bar.className="bar";
 const fill=document.createElement("i");bar.append(fill);box.append(n,bar);const st=document.createElement("span");st.className="size";
 st.textContent=size(file.size);row.append(box,st);$("sent").append(row);const x=new XMLHttpRequest();
 x.open("POST","api/upload?name="+encodeURIComponent(file.name));
 x.upload.onprogress=e=>{if(e.lengthComputable)fill.style.width=(100*e.loaded/e.total).toFixed(1)+"%";};
 x.onload=()=>{let msg="";try{msg=JSON.parse(x.responseText).error||"";}catch(e){}
  if(x.status===200){fill.style.width="100%";st.textContent="Sent";st.className="size ok";}else{st.textContent=msg||"Failed";st.className="size err";}done();};
 x.onerror=()=>{st.textContent="Failed";st.className="size err";done();};x.send(file);});}
$("choose").onclick=()=>$("pick").click();
$("pick").onchange=async()=>{if(busy)return;busy=true;$("choose").disabled=true;for(const f of $("pick").files)await upload(f);
 $("pick").value="";busy=false;$("choose").disabled=false;};
poll();timer=setInterval(poll,2000);
</script></body></html>)PAGE";

} // namespace eflow
