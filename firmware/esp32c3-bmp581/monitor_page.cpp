#include "monitor_page.h"

#include <Arduino.h>

const char MONITOR_PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>pressure-monitor</title>
<style>
:root{--bg:#fff;--fg:#222;--sub:#666;--line:#222;--grid:#ccc;--btn:#eee;--acc:#36c}
@media (prefers-color-scheme:dark){:root{--bg:#111;--fg:#eee;--sub:#aaa;--line:#eee;--grid:#444;--btn:#2a2a30;--acc:#6af}}
html,body{margin:0;height:100%;background:var(--bg);color:var(--fg);font-family:sans-serif}
body{display:flex;flex-direction:column;height:100vh;height:100dvh;
 padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left);
 box-sizing:border-box}
#top{position:relative;padding:8px 12px 4px;display:flex;flex-wrap:wrap;gap:6px 16px;align-items:flex-end}
.vals{display:flex;flex-wrap:wrap;gap:4px 24px;flex:1 1 auto;padding-right:84px}
.v{display:flex;align-items:baseline;gap:6px;white-space:nowrap}
.lb{font-size:.85em;color:var(--sub)}
.num{font-size:clamp(30px,10vw,60px);font-weight:bold;font-variant-numeric:tabular-nums;line-height:1.1}
.u{font-size:.9em;color:var(--sub)}
.ctl{display:flex;flex-wrap:wrap;gap:6px;align-items:center}
.seg{display:flex}
.seg button{border-radius:0;margin-left:-1px}
.seg button:first-child{border-radius:6px 0 0 6px}
.seg button:last-child{border-radius:0 6px 6px 0}
button,a.btn{font-size:.95em;padding:7px 10px;border:1px solid var(--grid);background:var(--btn);color:var(--fg);
 border-radius:6px;text-decoration:none;cursor:pointer;white-space:nowrap}
.seg button.on{background:var(--acc);border-color:var(--acc);color:#fff}
a.btn{font-weight:bold}
#set{position:absolute;top:8px;right:12px}
#gw{flex:1 1 auto;min-height:120px;position:relative;margin:0 4px}
canvas{position:absolute;left:0;top:0;width:100%;height:100%;display:block}
#sc{position:absolute;left:8px;top:4px;font-size:.8em;color:var(--sub)}
#bot{display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:4px 8px;padding:4px 12px 6px}
#st{font-size:.8em;color:var(--sub)}
.rc{display:flex;gap:6px;align-items:center}
#rec.on{background:#c33;border-color:#c33;color:#fff}
/* 横向き (高さが低い) ときは、数値を小さく 1 行にして、グラフを広くする */
@media (orientation:landscape) and (max-height:520px){
 #top{padding:4px 10px 2px;gap:4px 10px;align-items:center}
 #set{position:static;order:3}
 .vals{order:1}
 .ctl{order:2}
 .num{font-size:22px}
 .vals{flex-wrap:nowrap;padding-right:0;gap:4px 12px}
 .ctl{flex-wrap:nowrap}
 button,a.btn{padding:4px 7px;font-size:.82em}
 .seg button{padding:4px 6px}
 #dh{display:none}
 #bot{padding:2px 10px 2px;flex-wrap:nowrap}
}
</style></head><body>
<div id="top">
 <a class="btn" id="set" href="/settings">⚙ 設定</a>
 <div class="vals">
  <div class="v"><span class="lb">気圧</span><span id="hpa" class="num">----.--</span><span class="u">hPa</span></div>
  <div class="v"><span class="lb">変化量</span><span id="dp" class="num">--.--</span><span class="u">Pa</span>
   <span id="dh" class="u"></span></div>
 </div>
 <div class="ctl">
  <div class="seg" id="gsel"><button type="button" data-g="d" class="on">変化量</button><button type="button" data-g="p">気圧</button></div>
  <div class="seg" id="wsel"><button type="button" data-w="10" class="on">10秒</button><button type="button" data-w="60">60秒</button></div>
  <button type="button" id="fs" hidden>全画面</button>
 </div>
</div>
<div id="gw"><canvas id="cv"></canvas><div id="sc"></div></div>
<div id="bot"><span id="st">接続しています...</span>
 <span class="rc"><span id="ri" class="u"></span><button type="button" id="rec">● 記録</button>
 <button type="button" id="dl">CSV 保存</button><button type="button" id="clr">消去</button></span></div>
<script>
var MODES=['標準','ドライブ','カスタム','固定色'];
var MAXN=60*30+60;                 // 60 秒分 (30Hz) と少しの余裕
var buf=[],last=0,temp=0,mode=0,ws=null,graph='d',win=10,dirty=true;
function sel(id,attr,cb){var bs=document.querySelectorAll('#'+id+' button');
 for(var i=0;i<bs.length;i++)bs[i].onclick=function(){
  for(var j=0;j<bs.length;j++)bs[j].className='';this.className='on';cb(this.dataset[attr]);dirty=true;};}
sel('gsel','g',function(v){graph=v;});
sel('wsel','w',function(v){win=+v;});

// ---- 受信
function connect(){
 try{ws=new WebSocket('ws://'+location.host+'/ws');}catch(e){setTimeout(connect,1000);return;}
 ws.binaryType='arraybuffer';
 ws.onmessage=function(e){if(typeof e.data!=='string')onFrame(e.data);};
 ws.onclose=function(){ws=null;setTimeout(connect,1000);};
 ws.onerror=function(){try{ws.close();}catch(x){}};
}
var offset=null;   // 基板の millis -> スマホの時計
function onFrame(b){
 var dv=new DataView(b);if(dv.byteLength<8||dv.getUint8(0)!==77||dv.getUint8(1)!==1)return;
 var n=dv.getUint8(2);mode=dv.getUint8(3);temp=dv.getInt16(4,true)/100;
 n=Math.min(n,Math.floor((dv.byteLength-8)/16));if(n<1)return;
 // 最後のサンプルの時刻を、受け取った時刻とみなす。遅れの最も小さい値に合わせ、
 // 時計のずれにはゆっくり追従する (基板が再起動したら、合わせ直す)
 var now=Date.now(),off=now-dv.getUint32(8+(n-1)*16,true);
 if(offset===null||off<offset||off-offset>2000)offset=off;else offset+=(off-offset)*0.001;
 for(var i=0;i<n;i++){var o=8+i*16;
  var s={t:dv.getUint32(o,true),raw:dv.getInt32(o+4,true),hpf:dv.getInt32(o+8,true),
   r:dv.getUint8(o+12),g:dv.getUint8(o+13),b:dv.getUint8(o+14)};
  s.c='rgb('+s.r+','+s.g+','+s.b+')';buf.push(s);
  if(recording)addRow(s.t+offset,s.t,s.raw,s.hpf,Math.round(temp*100),(s.r<<16)|(s.g<<8)|s.b,mode);}
 if(buf.length>MAXN)buf.splice(0,buf.length-MAXN);
 last=Date.now();dirty=true;
}

// ---- 数値 (1 秒に 5 回。ちらつかないよう、直近の数サンプルを平均する)
function avg(k,f){var s=0,m=Math.min(k,buf.length);for(var i=buf.length-m;i<buf.length;i++)s+=f(buf[i]);return m?s/m:0;}
function upd(){
 var st=document.getElementById('st');
 if(!ws||ws.readyState!==1){st.textContent='基板に接続していません (再接続しています...)';return;}
 if(!buf.length||Date.now()-last>2000){st.textContent='データを待っています...';return;}
 var hpa=avg(6,function(s){return s.raw;})/6400,dp=avg(3,function(s){return s.hpf;})/64;
 document.getElementById('hpa').textContent=hpa.toFixed(2);
 document.getElementById('dp').textContent=(dp>=0?'+':'')+dp.toFixed(2);
 // 1 Pa ≒ 8.5 cm。気圧が上がる (+) のは、低くなったとき
 var cm=-dp*8.5;document.getElementById('dh').textContent='(高さ換算 '+(cm>=0?'+':'')+cm.toFixed(0)+' cm)';
 st.textContent='温度 '+temp.toFixed(1)+' ℃ / モード '+(mode+1)+' '+(MODES[mode]||'');
}
setInterval(upd,200);

// ---- グラフ
var cv=document.getElementById('cv'),css=getComputedStyle(document.documentElement);
function col(n){return css.getPropertyValue(n).trim();}
function nice(m){var s=[64,128,320,640,1280,3200,6400,12800,32000,64000,128000,320000,640000];
 for(var i=0;i<s.length;i++)if(m<=s[i])return s[i];return m;}
function draw(){
 requestAnimationFrame(draw);
 var dpr=window.devicePixelRatio||1,w=cv.clientWidth,h=cv.clientHeight;
 if(cv.width!==Math.round(w*dpr)||cv.height!==Math.round(h*dpr)){cv.width=Math.round(w*dpr);cv.height=Math.round(h*dpr);dirty=true;}
 if(!dirty)return;dirty=false;
 var c=cv.getContext('2d');c.setTransform(dpr,0,0,dpr,0,0);c.clearRect(0,0,w,h);
 if(!buf.length)return;
 var tEnd=buf[buf.length-1].t,tStart=tEnd-win*1000,i0=buf.length-1;
 while(i0>0&&(buf[i0-1].t-tStart)>=0)i0--;
 var strip=graph==='d'?12:0,gh=h-strip-4,x=function(t){return (t-tStart)/(win*1000)*w;};
 var lo,hi,label;
 if(graph==='d'){
  var m=0;for(var i=i0;i<buf.length;i++){var a=Math.abs(buf[i].hpf);if(a>m)m=a;}
  var sc=nice(Math.max(m,64));lo=-sc;hi=sc;label='変化量 ±'+(sc/64).toFixed(0)+' Pa';
 }else{
  lo=Infinity;hi=-Infinity;for(i=i0;i<buf.length;i++){var r=buf[i].raw;if(r<lo)lo=r;if(r>hi)hi=r;}
  var mid=(lo+hi)/2,span=Math.max(hi-lo,128)*1.1;lo=mid-span/2;hi=mid+span/2;
  label='気圧 '+(lo/6400).toFixed(2)+' 〜 '+(hi/6400).toFixed(2)+' hPa';
 }
 var y=function(v){return 2+(hi-v)/(hi-lo)*(gh-4);};
 // 目盛り (中央と上下)
 c.strokeStyle=col('--grid');c.lineWidth=1;c.beginPath();
 [lo,(lo+hi)/2,hi].forEach(function(v){c.moveTo(0,y(v));c.lineTo(w,y(v));});c.stroke();
 // 下の帯: LED1 の色
 if(strip){for(i=i0;i<buf.length;i++){var x0=x(buf[i].t),x1=i+1<buf.length?x(buf[i+1].t):w;
  c.fillStyle=buf[i].c;c.fillRect(x0,h-strip,Math.max(1,x1-x0+0.5),strip);}}
 c.strokeStyle=col('--line');c.lineWidth=2;c.beginPath();
 for(i=i0;i<buf.length;i++){var v=graph==='d'?buf[i].hpf:buf[i].raw,px=x(buf[i].t),py=y(v);
  if(i===i0)c.moveTo(px,py);else c.lineTo(px,py);}
 c.stroke();
 document.getElementById('sc').textContent=label+' / 直近 '+win+' 秒';
}
requestAnimationFrame(draw);

// ---- 全画面 (対応しているブラウザだけ。Android では横向きに固定を試みる)
var fsb=document.getElementById('fs'),de=document.documentElement;
if(de.requestFullscreen){fsb.hidden=false;
 fsb.onclick=function(){
  if(document.fullscreenElement){document.exitFullscreen();return;}
  de.requestFullscreen().then(function(){
   if(screen.orientation&&screen.orientation.lock)screen.orientation.lock('landscape').catch(function(){});
  }).catch(function(){});};
 document.addEventListener('fullscreenchange',function(){fsb.textContent=document.fullscreenElement?'全画面を終了':'全画面';});
}

// ---- 画面を点けたままにする
var wl=null;
function lock(){if(wl||!('wakeLock' in navigator)||document.visibilityState!=='visible')return;
 navigator.wakeLock.request('screen').then(function(l){wl=l;l.addEventListener('release',function(){wl=null;});}).catch(function(){});}
document.addEventListener('visibilitychange',function(){if(document.visibilityState==='visible')lock();});
document.addEventListener('click',lock);
// ---- 記録 (IndexedDB。使えない場合はメモリ)
// 1 行 = 7 個の整数: 時刻 (チャンクの先頭からの ms), dev_ms, raw, hpf, 温度 x100, 色 (RGB), mode
var COLS=7,CH=4096,chunk=null,chunkN=0,chunkBase=0,total=0,recording=false,db=null,mem=[];
function addRow(t,ms,raw,hpf,t100,rgb,md){
 if(!chunk){chunk=new Int32Array(CH*COLS);chunkN=0;chunkBase=Math.round(t);}
 var i=chunkN*COLS;
 chunk[i]=Math.round(t)-chunkBase;chunk[i+1]=ms|0;chunk[i+2]=raw;chunk[i+3]=hpf;
 chunk[i+4]=t100;chunk[i+5]=rgb;chunk[i+6]=md;
 chunkN++;total++;
 if(chunkN>=CH)saveChunk();
}
function saveChunk(){
 if(!chunk||chunkN===0)return;
 var rec={base:chunkBase,n:chunkN,buf:chunk.slice(0,chunkN*COLS).buffer};chunk=null;chunkN=0;
 if(db){try{db.transaction('c','readwrite').objectStore('c').add(rec);return;}catch(e){}}
 mem.push(rec);
}
setInterval(function(){if(recording)saveChunk();},5000);
// ハブの画面と同じアドレス (192.168.4.1) で開くことがあるので、別の名前のデータベースにする
function openDb(cb){
 try{var rq=indexedDB.open('pressure-monitor-log',1);
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
function download(){
 saveChunk();
 var parts=['\ufefftime,unix_ms,dev_ms,pressure_hPa,raw,hpf,temperature_C,led_r,led_g,led_b,mode\n'];
 eachChunk(function(rec){
  var a=new Int32Array(rec.buf),s=[];
  for(var r=0;r<rec.n;r++){var i=r*COLS,t=rec.base+a[i],rgb=a[i+5];
   s.push(ts(t)+','+t+','+(a[i+1]>>>0)+','+(a[i+2]/6400).toFixed(4)+','+a[i+2]+','+a[i+3]+','
    +(a[i+4]/100).toFixed(2)+','+((rgb>>16)&255)+','+((rgb>>8)&255)+','+(rgb&255)+','+(a[i+6]+1)+'\n');}
  parts.push(s.join(''));
 },function(){
  var d=new Date(),fn='pressure-monitor-'+d.getFullYear()+pad(d.getMonth()+1,2)+pad(d.getDate(),2)+'-'
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
function recUi(){recBtn.textContent=recording?'■ 停止':'● 記録';recBtn.className=recording?'on':'';
 document.getElementById('ri').textContent=(recording?'記録中 ':'')+total+' 行';}
recBtn.onclick=function(){recording=!recording;if(!recording)saveChunk();recUi();};
document.getElementById('dl').onclick=download;
document.getElementById('clr').onclick=function(){clearAll();recUi();};
setInterval(recUi,500);

openDb(function(){eachChunk(function(rec){total+=rec.n;},function(){recUi();});});
connect();lock();
</script></body></html>
)HTML";
