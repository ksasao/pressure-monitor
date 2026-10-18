#include "hub_page.h"

#include <Arduino.h>

const char HUB_PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>pressure-hub</title>
<style>
:root{--bg:#fff;--fg:#222;--sub:#666;--card:#f2f2f5;--line:#222;--grid:#bbb;--acc:#36c}
@media (prefers-color-scheme:dark){:root{--bg:#111;--fg:#eee;--sub:#aaa;--card:#1e1e24;--line:#eee;--grid:#555;--acc:#6af}}
body{font-family:sans-serif;margin:12px;background:var(--bg);color:var(--fg)}
h1{font-size:1.15em;margin:0 0 6px}
.bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:8px}
button{font-size:1em;padding:8px 12px}
#rec.on{background:#c33;color:#fff;border-color:#c33}
.sub{color:var(--sub);font-size:.85em}
#wv{display:none;margin:0 0 8px;padding:8px 10px;border-radius:8px;background:#fff3cd;color:#5c4400;font-size:.85em;line-height:1.5}
#wv.on{display:block}
#wv a,#wv button{font-size:1em;margin:4px 6px 0 0;padding:5px 10px;border-radius:6px;border:1px solid #c9a227;background:#ffe08a;color:#3d2e00;text-decoration:none;display:inline-block;cursor:pointer}
#wv .x{float:right;background:none;border:0;margin:0;padding:0 4px;font-size:1.2em}

#grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:10px}
.card{background:var(--card);border-radius:8px;padding:8px;border:2px solid transparent}
.card.stale{opacity:.45}
.card.drag{border-color:var(--acc);opacity:.85;box-shadow:0 4px 14px rgba(0,0,0,.25)}
.hd{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.hdl{cursor:grab;touch-action:none;user-select:none;-webkit-user-select:none;font-size:1.4em;
 line-height:1;padding:2px 6px;color:var(--sub)}
.card.drag .hdl{cursor:grabbing}
.nm{font-weight:bold;font-size:1.15em;word-break:break-all}
.leds{display:flex;gap:4px}
.leds span{width:18px;height:18px;border-radius:50%;background:#000;border:1px solid #777}
.warn{color:#c33;font-size:.85em}
canvas{width:100%;height:90px;display:block;margin-top:6px}
.kv{font-size:.85em;color:var(--sub);margin-top:4px;line-height:1.5}
</style></head><body>
<h1>pressure-hub</h1>
<div id="wv"><button type="button" class="x" onclick="this.parentNode.className=''">×</button>
<b>この画面（Wi-Fi のログイン画面）では、記録を CSV に保存できません。</b><br>
記録するときは、Chrome を開いて <b><span class="wvurl"></span></b> を表示してください
（開けないときは、モバイルデータ通信をオフにしてください）。<br>
<a class="wvopen" href="#">Chrome で開く</a><button type="button" class="wvcopy">アドレスをコピー</button></div>
<div class="bar"><span id="conn" class="sub">接続しています...</span></div>
<div class="bar"><span id="net" class="sub"></span></div>
<div class="bar">
<button id="rec" type="button">記録を開始</button>
<button id="dl" type="button">CSV を保存</button>
<button id="clr" type="button">記録を消去</button>
<span id="recinfo" class="sub"></span>
</div>
<div class="bar"><button id="sortname" type="button">名前順に並べる</button>
<span class="sub">左上の ⠿ をドラッグすると、並べ替えられます (このブラウザに保存されます)。</span></div>
<div id="grid"></div>
<p class="sub">記録は、このブラウザの中に保存され、再読み込みしても残ります。
画面を消したり、別のアプリに切り替えたりすると、その間は受信も記録もされません。
グラフは HPF の出力 (直近 10 秒)、下の帯は LED1 の色の履歴です。</p>
<script>
var HIST=300,MODES=['標準','ドライブ','カスタム','固定色'],VER=3,HDR=32;
var nodes={},offset=null,lastMsg=0,hubDropped=0,ws=null;
var grid=document.getElementById('grid');
var dec=('TextDecoder' in window)?new TextDecoder('utf-8'):null;

// ---- 並び順 (MAC アドレスの並び。このブラウザに保存する)
var order=[];
try{order=JSON.parse(localStorage.getItem('hubOrder')||'[]')||[];}catch(e){order=[];}
function saveOrder(){
 order=[].map.call(grid.children,function(el){return el.dataset.mac;});
 try{localStorage.setItem('hubOrder',JSON.stringify(order));}catch(e){}
}
function byName(x,y){return x.name.localeCompare(y.name)||(x.mac<y.mac?-1:1);}
function placeCards(){
 if(drag)return;            // ドラッグ中は動かさない (離したあとに並べ直す)
 var a=Object.keys(nodes).map(function(k){return nodes[k];});
 a.sort(function(x,y){
  var i=order.indexOf(x.mac),j=order.indexOf(y.mac);
  if(i>=0&&j>=0)return i-j;
  if(i>=0)return -1;if(j>=0)return 1;      // 並べ替えたことのない装置は、後ろに名前順
  return byName(x,y);});
 a.forEach(function(n){grid.appendChild(n.el);});
}
document.getElementById('sortname').onclick=function(){
 order=[];try{localStorage.removeItem('hubOrder');}catch(e){}placeCards();};

// ---- ドラッグで並べ替え (マウスとタッチの両方。⠿ をつかむ)
var drag=null;
function startDrag(n,e){
 if(e.button>0)return;
 e.preventDefault();drag={n:n};n.el.classList.add('drag');
 try{e.target.setPointerCapture(e.pointerId);}catch(x){}
}
document.addEventListener('pointermove',function(e){
 if(!drag)return;e.preventDefault();
 // 画面の端に近づいたら、スクロールする
 if(e.clientY<50)window.scrollBy(0,-12);else if(e.clientY>window.innerHeight-50)window.scrollBy(0,12);
 var el=document.elementFromPoint(e.clientX,e.clientY),card=el&&el.closest?el.closest('.card'):null;
 if(!card||card===drag.n.el||card.parentNode!==grid)return;
 var cards=[].slice.call(grid.children),from=cards.indexOf(drag.n.el),to=cards.indexOf(card);
 grid.insertBefore(drag.n.el,from<to?card.nextSibling:card);
},{passive:false});
function endDrag(){if(!drag)return;drag.n.el.classList.remove('drag');drag=null;saveOrder();}
document.addEventListener('pointerup',endDrag);
document.addEventListener('pointercancel',endDrag);

// ---- 受信
function hex2(v){return (v<16?'0':'')+v.toString(16);}
function macStr(dv,o){var s=[];for(var i=0;i<6;i++)s.push(hex2(dv.getUint8(o+i)));return s.join(':');}
function readName(buf,o){
 var b=new Uint8Array(buf,o,16),n=b.indexOf(0);if(n<0)n=16;
 if(dec)return dec.decode(b.subarray(0,n));
 var s='';for(var i=0;i<n;i++)s+=String.fromCharCode(b[i]);return s;
}
function connect(){
 try{ws=new WebSocket('ws://'+location.host+'/ws');}catch(e){setTimeout(connect,1000);return;}
 ws.binaryType='arraybuffer';
 ws.onmessage=function(e){onFrame(e.data);};
 ws.onclose=function(){ws=null;setTimeout(connect,1000);};
 ws.onerror=function(){try{ws.close();}catch(x){}};
}
function getNode(mac,name){
 var n=nodes[mac];if(n)return n;
 var c=document.createElement('div');c.className='card';c.dataset.mac=mac;
 c.innerHTML='<div class="hd"><span class="hdl" title="ドラッグで並べ替え">⠿</span><span class="nm"></span>'
  +'<span class="leds">'+'<span></span>'.repeat(5)+'</span>'
  +'<span class="sub md"></span><span class="warn"></span></div><canvas></canvas><div class="kv"></div>';
 n=nodes[mac]={mac:mac,name:name,el:c,hpf:[],col:[],seq:null,pkts:0,lost:0,last:0,
  raw:0,temp:0,bat:0,rssi:0,mode:0};
 c.querySelector('.hdl').addEventListener('pointerdown',function(e){startDrag(n,e);});
 placeCards();return n;
}
function onFrame(buf){
 var dv=new DataView(buf);if(dv.byteLength<12||dv.getUint8(0)!==72)return;
 var cnt=dv.getUint16(2,true),hubMs=dv.getUint32(4,true);hubDropped=dv.getUint32(8,true);
 var now=Date.now();lastMsg=now;
 // ハブの millis -> スマホの時計。遅れの最も小さい値に合わせ、時計のずれにはゆっくり追従する
 var off=now-hubMs;
 if(offset===null||off<offset||off-offset>2000)offset=off;else offset+=(off-offset)*0.001;
 var o=12;
 for(var k=0;k<cnt;k++){
  if(o+12>dv.byteLength)break;
  var rxMs=dv.getUint32(o,true),rssi=dv.getInt8(o+4),len=dv.getUint8(o+5),mac=macStr(dv,o+6);
  var p=o+12;o=p+len;
  if(o>dv.byteLength||len<HDR)break;
  if(dv.getUint8(p)!==80||dv.getUint8(p+1)!==77||dv.getUint8(p+2)!==VER)continue;
  var mode=dv.getUint8(p+4),ns=dv.getUint8(p+5),seq=dv.getUint16(p+6,true);
  var bat=dv.getUint16(p+8,true),temp=dv.getInt16(p+10,true)/100,name=readName(buf,p+16);
  ns=Math.min(ns,Math.floor((len-HDR)/16));if(ns<1)continue;
  var nd=getNode(mac,name);
  if(nd.name!==name){nd.name=name;placeCards();}
  if(nd.seq!==null){var gap=(seq-nd.seq-1+65536)%65536;if(gap<1000)nd.lost+=gap;}
  nd.seq=seq;nd.pkts++;nd.mode=mode;nd.bat=bat;nd.temp=temp;nd.rssi=rssi;nd.last=now;
  var lastMs=dv.getUint32(p+HDR+(ns-1)*16,true);
  for(var i=0;i<ns;i++){
   var q=p+HDR+i*16;
   var ms=dv.getUint32(q,true),raw=dv.getInt32(q+4,true),hpf=dv.getInt32(q+8,true);
   var r=dv.getUint8(q+12),g=dv.getUint8(q+13),b=dv.getUint8(q+14);
   var hubT=rxMs-((lastMs-ms)>>>0);
   nd.hpf.push(hpf);nd.col.push('rgb('+r+','+g+','+b+')');
   if(nd.hpf.length>HIST){nd.hpf.shift();nd.col.shift();}
   nd.raw=raw;
   if(recording)addRow(hubT+offset,hubT,nameIndex(name,mac),seq,ms,raw,hpf,Math.round(temp*100),bat,rssi,
    (r<<16)|(g<<8)|b,mode);
  }
 }
}

// ---- 表示
var css=getComputedStyle(document.documentElement);
function niceScale(m){var s=[64,128,256,640,1280,3200,6400,12800,32000,64000,128000,320000,640000];
 for(var i=0;i<s.length;i++)if(m<=s[i])return s[i];return m;}
function draw(n){
 var cv=n.el.querySelector('canvas'),dpr=window.devicePixelRatio||1;
 var w=cv.clientWidth,h=cv.clientHeight;
 if(cv.width!==Math.round(w*dpr)){cv.width=Math.round(w*dpr);cv.height=Math.round(h*dpr);}
 var c=cv.getContext('2d');c.setTransform(dpr,0,0,dpr,0,0);c.clearRect(0,0,w,h);
 var a=n.hpf,len=a.length,strip=10,gh=h-strip-2,mx=0;
 for(var i=0;i<len;i++){var v=Math.abs(a[i]);if(v>mx)mx=v;}
 var sc=niceScale(Math.max(mx,64)),dx=w/(HIST-1),x0=w-(len-1)*dx;
 c.strokeStyle=css.getPropertyValue('--grid').trim();c.lineWidth=1;
 c.beginPath();c.moveTo(0,gh/2);c.lineTo(w,gh/2);c.stroke();
 for(i=0;i<len;i++){c.fillStyle=n.col[i];c.fillRect(x0+i*dx-dx/2,h-strip,dx+1,strip);}
 c.strokeStyle=css.getPropertyValue('--line').trim();c.lineWidth=1.5;c.beginPath();
 for(i=0;i<len;i++){var y=gh/2-a[i]/sc*(gh/2-2);if(i)c.lineTo(x0+i*dx,y);else c.moveTo(x0+i*dx,y);}
 c.stroke();
 n.scale=sc;
}
function render(){
 var now=Date.now(),list=Object.keys(nodes).map(function(k){return nodes[k];}),cnt={},live=0;
 list.forEach(function(n){cnt[n.name]=(cnt[n.name]||0)+1;});
 list.forEach(function(n){
  var age=(now-n.last)/1000,stale=age>3;if(!stale)live++;
  n.el.classList.toggle('stale',stale);
  n.el.querySelector('.nm').textContent=n.name||'(名前なし)';
  var leds=n.el.querySelectorAll('.leds span');
  for(var i=0;i<5;i++){var c=n.col[n.col.length-1-i];leds[i].style.background=c||'#000';}
  n.el.querySelector('.md').textContent=(n.rssi===0?'ハブ / ':'')+(MODES[n.mode]||'');
  n.el.querySelector('.warn').textContent=cnt[n.name]>1?'名前が重複しています':'';
  draw(n);
  var lossPct=(n.pkts+n.lost)?(100*n.lost/(n.pkts+n.lost)):0;
  n.el.querySelector('.kv').innerHTML=
   (n.raw/6400).toFixed(2)+' hPa / '+n.temp.toFixed(1)+' ℃ / 電池 '
   +(n.bat<1000?'なし':(n.bat/1000).toFixed(2)+' V')+'<br>'
   +(n.rssi===0?'':'電波 '+n.rssi+' dBm / ')+'欠落 '+lossPct.toFixed(1)+' % ('+n.lost+'/'+(n.pkts+n.lost)+')'
   +' / グラフ ±'+(n.scale/64).toFixed(0)+' Pa'+(stale?' / '+age.toFixed(0)+' 秒前':'')
   +'<br><span style="font-size:.9em">'+n.mac+'</span>';
 });
 var c=document.getElementById('conn');
 if(!ws||ws.readyState!==1)c.textContent='ハブに接続していません (再接続しています...)';
 else if(now-lastMsg>3000)c.textContent='ハブからの受信が止まっています';
 else c.textContent='受信中: '+live+' 台 / ハブの取りこぼし '+hubDropped;
 document.getElementById('recinfo').textContent=(recording?'記録中: ':'記録: ')+total+' 行';
}
setInterval(render,100);

// ---- ネットワークの状況 (ルーターから割り当てられた IP アドレスなど)
function esc(s){return String(s).replace(/[&<>"']/g,function(c){return '&#'+c.charCodeAt(0)+';';});}
function link(u){return '<a href="'+esc(u)+'">'+esc(u)+'</a>';}
function info(){fetch('/info').then(function(r){return r.json();}).then(function(j){
 var s=[];
 if(j.net==='station'){s.push('Wi-Fi「'+esc(j.ssid)+'」に接続中: '+link('http://'+j.sta_ip+'/')
  +(j.mdns?' または '+link('http://'+j.mdns+'/'):''));}
 else if(j.net==='connecting'){s.push('Wi-Fi「'+esc(j.ssid)+'」に接続しています...');}
 if(j.ap_ssid)s.push('アクセスポイント '+esc(j.ap_ssid)+': '+link('http://'+j.ap_ip+'/'));
 s.push('グループ '+j.group+' / チャンネル '+j.channel+' / ファームウェア '+esc(j.fw||''));
 document.getElementById('net').innerHTML=s.join(' / ');
}).catch(function(){});}
setInterval(info,5000);info();

// ---- 記録 (IndexedDB。使えない場合はメモリ)
// 1 行 = 12 個の整数: 時刻 (チャンクの先頭からの ms), hub_ms, 名前の番号, seq, dev_ms, raw, hpf,
//                     温度 x100, 電池 mV, rssi, 色 (RGB), mode
// 名前の番号は、チャンクごとに持つ表 (names: [{name, mac}]) の位置
var COLS=12,CH=4096,chunk=null,chunkN=0,chunkBase=0,total=0,recording=false,db=null,mem=[];
var names=[],nameMap={};
function nameIndex(name,mac){
 var k=mac+'|'+name,i=nameMap[k];
 if(i===undefined){i=nameMap[k]=names.length;names.push({name:name,mac:mac});}
 return i;
}
function addRow(t,hubT,ni,seq,ms,raw,hpf,t100,bat,rssi,rgb,mode){
 if(!chunk){chunk=new Int32Array(CH*COLS);chunkN=0;chunkBase=Math.round(t);}
 var i=chunkN*COLS;
 chunk[i]=Math.round(t)-chunkBase;chunk[i+1]=hubT|0;chunk[i+2]=ni;chunk[i+3]=seq;chunk[i+4]=ms|0;
 chunk[i+5]=raw;chunk[i+6]=hpf;chunk[i+7]=t100;chunk[i+8]=bat;chunk[i+9]=rssi;chunk[i+10]=rgb;chunk[i+11]=mode;
 chunkN++;total++;
 if(chunkN>=CH)saveChunk();
}
function saveChunk(){
 if(!chunk||chunkN===0)return;
 var rec={base:chunkBase,n:chunkN,buf:chunk.slice(0,chunkN*COLS).buffer,names:names.slice()};
 chunk=null;chunkN=0;
 if(db){try{db.transaction('c','readwrite').objectStore('c').add(rec);return;}catch(e){}}
 mem.push(rec);
}
setInterval(function(){if(recording)saveChunk();},5000);
function openDb(cb){
 try{var rq=indexedDB.open('pressure-hub',1);
  rq.onupgradeneeded=function(){rq.result.createObjectStore('c',{autoIncrement:true});};
  rq.onsuccess=function(){db=rq.result;cb();};rq.onerror=function(){db=null;cb();};
 }catch(e){db=null;cb();}
}
function eachChunk(fn,done){
 mem.forEach(fn);
 if(!db){done();return;}
 var rq=db.transaction('c','readonly').objectStore('c').openCursor();
 rq.onsuccess=function(){var c=rq.result;if(c){fn(c.value);c.continue();}else done();};
 rq.onerror=function(){done();};
}
function pad(n,w){n=''+n;while(n.length<w)n='0'+n;return n;}
function ts(t){var d=new Date(t);return d.getFullYear()+'-'+pad(d.getMonth()+1,2)+'-'+pad(d.getDate(),2)+' '
 +pad(d.getHours(),2)+':'+pad(d.getMinutes(),2)+':'+pad(d.getSeconds(),2)+'.'+pad(d.getMilliseconds(),3);}
function csvq(s){s=String(s);return /[",\r\n]/.test(s)?'"'+s.replace(/"/g,'""')+'"':s;}
function download(){
 saveChunk();
 // Excel で文字化けしないよう、先頭に BOM を付ける (名前に日本語を使える)
 var parts=['﻿time,unix_ms,name,mac,seq,dev_ms,hub_ms,pressure_hPa,raw,hpf,temperature_C,battery_mV,rssi_dBm,led_r,led_g,led_b,mode\n'];
 eachChunk(function(rec){
  var a=new Int32Array(rec.buf),s=[],tbl=rec.names||[];
  for(var r=0;r<rec.n;r++){var i=r*COLS,t=rec.base+a[i],rgb=a[i+10],nm=tbl[a[i+2]]||{name:''+a[i+2],mac:''};
   s.push(ts(t)+','+t+','+csvq(nm.name)+','+nm.mac+','+a[i+3]+','+(a[i+4]>>>0)+','+(a[i+1]>>>0)+','
    +(a[i+5]/6400).toFixed(4)+','+a[i+5]+','+a[i+6]+','+(a[i+7]/100).toFixed(2)+','+a[i+8]+','+a[i+9]+','
    +((rgb>>16)&255)+','+((rgb>>8)&255)+','+(rgb&255)+','+(a[i+11]+1)+'\n');}
  parts.push(s.join(''));
 },function(){
  var d=new Date(),fn='pressure-hub-'+d.getFullYear()+pad(d.getMonth()+1,2)+pad(d.getDate(),2)+'-'
   +pad(d.getHours(),2)+pad(d.getMinutes(),2)+pad(d.getSeconds(),2)+'.csv';
  var url=URL.createObjectURL(new Blob(parts,{type:'text/csv'}));
  var a=document.createElement('a');a.href=url;a.download=fn;document.body.appendChild(a);a.click();
  setTimeout(function(){URL.revokeObjectURL(url);a.remove();},10000);
 });
}
function clearAll(){
 if(!confirm('記録を全て消去します。よろしいですか？'))return;
 chunk=null;chunkN=0;mem=[];total=0;
 if(db){try{db.transaction('c','readwrite').objectStore('c').clear();}catch(e){}}
}
var recBtn=document.getElementById('rec');
// ---- Android の Wi-Fi のログイン画面 (WebView) の検出。
// ここでは、ブラウザのダウンロード (blob:) が使えず、「保存できませんでした」になる。
// また、記録はブラウザごとに別の場所に保存されるので、Chrome に移せない。
// そのため、記録を始める前に、Chrome で開き直すよう案内する
var inWebView=/; wv\)/.test(navigator.userAgent);
var pageUrl='http://'+location.host+location.pathname;
(function(){
 var w=document.getElementById('wv');
 w.querySelector('.wvurl').textContent=pageUrl;
 w.querySelector('.wvopen').href='intent://'+location.host+location.pathname+'#Intent;scheme=http;package=com.android.chrome;end';
 w.querySelector('.wvcopy').onclick=function(){
  var t=document.createElement('textarea');t.value=pageUrl;document.body.appendChild(t);t.select();
  var ok=false;try{ok=document.execCommand('copy');}catch(e){}t.remove();
  this.textContent=ok?'コピーしました':'コピーできませんでした';};
 if(inWebView)w.className='on';
})();
function webViewBlocked(){
 if(!inWebView)return false;
 var w=document.getElementById('wv');w.className='on';w.scrollIntoView&&w.scrollIntoView();
 alert('この画面（Wi-Fi のログイン画面）では、CSV を保存できません。\nChrome で '+pageUrl+' を開いてから、記録してください。');
 return true;
}
recBtn.onclick=function(){if(!recording&&webViewBlocked())return;recording=!recording;if(!recording)saveChunk();
 recBtn.textContent=recording?'記録を停止':'記録を開始';recBtn.className=recording?'on':'';lock();};
document.getElementById('dl').onclick=function(){if(webViewBlocked())return;lock();download();};
document.getElementById('clr').onclick=clearAll;

// ---- 画面を点けたままにする
var wl=null;
function lock(){if(wl||!('wakeLock' in navigator)||document.visibilityState!=='visible')return;
 navigator.wakeLock.request('screen').then(function(l){wl=l;l.addEventListener('release',function(){wl=null;});}).catch(function(){});}
document.addEventListener('visibilitychange',function(){if(document.visibilityState==='visible')lock();});
document.addEventListener('click',lock);

openDb(function(){eachChunk(function(rec){total+=rec.n;},function(){});connect();lock();});
</script></body></html>
)HTML";
