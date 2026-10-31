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
#wifioff{margin-left:auto}
#gw{flex:1 1 auto;min-height:120px;position:relative;margin:0 4px}
canvas{position:absolute;left:0;top:0;width:100%;height:100%;display:block}
#sc{position:absolute;left:8px;top:4px;font-size:.8em;color:var(--sub)}
#bot{display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:4px 8px;padding:4px 12px 6px}
#st{font-size:.8em;color:var(--sub)}
.rc{display:flex;gap:6px;align-items:center}
#rec.on{background:#c33;border-color:#c33;color:#fff}
#recdlg,#iosdlg{display:none;position:fixed;inset:0;background:rgba(0,0,0,.45);z-index:10;align-items:center;justify-content:center;padding:12px}
#recdlg.on,#iosdlg.on{display:flex}
#recdlg .box,#iosdlg .box{background:var(--bg);color:var(--fg);border-radius:10px;padding:14px;max-width:420px;line-height:1.55;font-size:.92em}
#recdlg .box button,#iosdlg .box button{display:block;width:100%;margin-top:8px;padding:10px;font-size:1em}
#recdlg small,#iosdlg small{color:var(--sub);display:block;margin-top:2px}
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
  <button type="button" id="wifioff">Wi-Fi を切る</button>
 </div>
</div>
<div id="gw"><canvas id="cv"></canvas><div id="sc"></div></div>
<div id="bot"><span id="st">接続しています...</span>
 <span class="rc"><span id="ri" class="u"></span><button type="button" id="rec">● 記録</button>
 <a class="btn" id="dl" href="/log.csv">CSV 保存</a><button type="button" id="clr">消去</button></span></div>
<div id="iosdlg"><div class="box">
 <b>この画面では、CSV を保存できません</b><br>
 iPhone の Wi-Fi のログイン画面（自動で開いた画面）には、ファイルを保存する機能がありません。
 記録は基板の中に残っています。下のボタンで接続を完了してから、ふだんのブラウザ（Chrome や Safari）で保存してください。
 <button type="button" id="iosauth">接続を完了して、ブラウザで保存する</button>
 <small>押すと、右上が「完了」に変わります。「完了」で閉じても、Wi-Fi はつながったままになるので、
  ブラウザで <b id="iosurl"></b> を開いて、「CSV 保存」から保存できます。</small>
 <button type="button" id="ioscopy">アドレスをコピー</button>
 <button type="button" id="iosclose">閉じる</button>
</div></div>
<div id="recdlg"><div class="box">
 <b>基板の中に記録します</b>
 <label style="display:block;margin-top:6px">記録の間隔
  <select id="reciv" style="font-size:1em;margin-left:4px">
   <option value="0">1/30 秒（最長 約 16 時間）</option>
   <option value="1000">1 秒（最長 約 68 時間）</option>
   <option value="30000">30 秒（最長 約 84 日）</option>
  </select></label>
 <small>1 秒と 30 秒は、その間の平均を記録します。LED の表示は変わりません。</small>
 <div id="recold"></div>
 <div id="recrole" style="display:none;border:1px solid #d08000;border-radius:8px;padding:8px;margin:8px 0">
  <b>役割が「<span id="recrolename"></span>」になっています</b><br>
  複数台の表示のための無線（ESP-NOW）が、Wi-Fi をオフにしても動き続けます。
  ライトスリープに入らないため、電池の消費が大きくなります（単 3 電池 2 本で 5 時間ほど）。
  <label style="display:block;margin-top:6px"><input type="checkbox" id="recsolo" checked> 記録の前に「単体」に切り替える（おすすめ）</label>
  <small>あとで、設定ページの「役割」で戻せます。</small>
 </div>
 <button type="button" id="recon">Wi-Fi のまま記録する</button>
 <small>このページで、グラフを見ながら記録します（LED5 が赤と水色で点滅します）。</small>
 <button type="button" id="recoff">記録して Wi-Fi をオフにする（省電力）</button>
 <small>基板が再起動し、Wi-Fi をオフにして記録を続けます（LED5 が赤く点滅します）。
  止めるときは、BOOT ボタンを長押しして設定モードに入り、このページの「■ 停止」を押してください。</small>
 <button type="button" id="reccancel">やめる</button>
</div></div>
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
var HDR=24;        // フレームのヘッダ (monitor_page.h)
var rec={on:false,full:false,none:false,count:0,cap:0,iv:0};
var dl={sent:0,total:0,t0:0,s0:0,lastSent:-1,lastMove:0};   // CSV のダウンロードの進み具合     // 基板の中への記録の状態 (devlog.h)
function onFrame(b){
 var dv=new DataView(b);if(dv.byteLength<HDR||dv.getUint8(0)!==77||dv.getUint8(1)!==3)return;
 var n=dv.getUint8(2);mode=dv.getUint8(3);temp=dv.getInt16(4,true)/100;
 var f=dv.getUint8(6);rec.on=!!(f&1);rec.full=!!(f&2);rec.none=!!(f&4);rec.count=dv.getUint32(8,true);rec.cap=dv.getUint32(12,true);rec.iv=dv.getUint8(7);
 dlUpdate(dv.getUint32(16,true),dv.getUint32(20,true));
 n=Math.min(n,Math.floor((dv.byteLength-HDR)/16));if(n<1)return;
 for(var i=0;i<n;i++){var o=HDR+i*16;
  var s={t:dv.getUint32(o,true),raw:dv.getInt32(o+4,true),hpf:dv.getInt32(o+8,true),
   r:dv.getUint8(o+12),g:dv.getUint8(o+13),b:dv.getUint8(o+14)};
  s.c='rgb('+s.r+','+s.g+','+s.b+')';buf.push(s);}
 if(buf.length>MAXN)buf.splice(0,buf.length-MAXN);
 last=Date.now();dirty=true;
}

// ---- 装置の名前 (/status。下の行と、ブラウザのタブに出す。CSV のファイル名にも使う)
var devName='';
function loadName(){
 fetch('/status').then(function(r){return r.json();}).then(function(j){
  devName=j.node||'';document.title=(devName?devName+' - ':'')+'pressure-monitor';
 }).catch(function(){setTimeout(loadName,3000);});
}
loadName();

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
 st.textContent=(devName?devName+' / ':'')+temp.toFixed(1)+' ℃ / モード '+(mode+1)+' '+(MODES[mode]||'');
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
// ---- 記録 (基板の中に記録する。devlog.h)
// ブラウザの中には記録しない。Android の Wi-Fi のログイン画面は、ブラウザの中で作ったファイルを
// 保存できないため。基板が返す /log.csv なら、ログイン画面でも保存できる。
// また、スマホの画面が消えても、基板が記録を続ける
function pad(n,w){n=''+n;while(n.length<w)n='0'+n;return n;}
function post(q){return fetch('/rec',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:q})
 .then(function(r){if(r.ok)return true;return r.text().then(function(t){alert(t||('エラー '+r.status));return false;});})
 .catch(function(){alert('基板に送れませんでした。もう一度押してください。');return false;});}
var recBtn=document.getElementById('rec'),dlA=document.getElementById('dl');
// 残りの行数を、時間にする (記録の間隔 rec.iv 秒。0 = 毎サンプル 30Hz)
function fmtRest(n){var s=n*(rec.iv||1/30);
 if(s<5400)return Math.floor(s/60)+' 分';if(s<172800)return Math.floor(s/3600)+' 時間';return Math.floor(s/86400)+' 日';}
function mb(b){return (b/1048576).toFixed(1);}
// 基板が送ったバイト数から、進み具合と残り時間を求める
function dlUpdate(sent,total){
 var now=Date.now();
 if(total!==dl.total||sent<dl.sent){dl.t0=now;dl.s0=sent;}   // 新しいダウンロード
 if(sent!==dl.lastSent){dl.lastSent=sent;dl.lastMove=now;}
 dl.sent=sent;dl.total=total;
}
function dlText(){
 if(!dl.total)return '';
 var now=Date.now();
 if(dl.sent>=dl.total)return now-dl.lastMove<10000?'CSV の送信が終わりました（'+mb(dl.total)+' MB）':'';
 if(now-dl.lastMove>15000)return 'CSV の送信が止まっています（'+mb(dl.sent)+' / '+mb(dl.total)+' MB）';
 var pct=Math.floor(100*dl.sent/dl.total),el=(now-dl.t0)/1000,rate=el>2?(dl.sent-dl.s0)/el:0;
 var rest=rate>0?Math.ceil((dl.total-dl.sent)/rate/60):0;
 return 'CSV 送信中 '+mb(dl.sent)+' / '+mb(dl.total)+' MB（'+pct+'%'+(rest?'、残り約 '+rest+' 分':'')+'）';
}
function recUi(){
 recBtn.textContent=rec.on?'■ 停止':'● 記録';recBtn.className=rec.on?'on':'';
 var t=rec.count.toLocaleString()+' 行';
 if(rec.on)t='記録中 '+t+'（残り約 '+fmtRest(rec.cap-rec.count)+'）';
 else if(rec.full)t='いっぱいで停止 '+t;
 if(rec.none)t='記録できません（記録の領域がありません）';
 var d=dlText();if(d)t=d;
 if(fast.text&&(fast.busy||Date.now()-fast.at<10000))t=fast.text;   // 記録の受信と、CSV の作成
 document.getElementById('ri').textContent=t;
}
// 停止するときも、スマホの時計を送る (基板の時計のずれを補正する)
var dlg=document.getElementById('recdlg');
recBtn.onclick=function(){
 if(rec.on){post('op=stop&t='+Date.now());return;}
 document.getElementById('recold').textContent=rec.count>0
  ?'前の記録（'+rec.count.toLocaleString()+' 行）は消えます。必要なら、先に「CSV 保存」を押してください。':'';
 // 無線の役割を調べる。送信・ハブのままだと、Wi-Fi をオフにしても ESP-NOW が動き続け、
 // 電池の消費が大きいので、単体に切り替えるかを、ダイアログで確かめる
 fetch('/status').then(function(r){return r.json();}).then(function(j){showRecDlg(j.role);})
  .catch(function(){showRecDlg('');});
};
var ROLE_NAMES={sender:'送信',hub:'ハブ'};
function showRecDlg(role){
 try{var v=localStorage.getItem('recIv');if(v)document.getElementById('reciv').value=v;}catch(e){}
 var warn=!!ROLE_NAMES[role];
 document.getElementById('recrole').style.display=warn?'block':'none';
 document.getElementById('recrolename').textContent=ROLE_NAMES[role]||'';
 document.getElementById('recsolo').checked=true;
 dlg.className='on';
}
function startRec(wifiOff){
 dlg.className='';
 var solo=document.getElementById('recrole').style.display!=='none'&&document.getElementById('recsolo').checked;
 var iv=document.getElementById('reciv').value;
 try{localStorage.setItem('recIv',iv);}catch(e){}
 post('op=start&t='+Date.now()+'&tz='+new Date().getTimezoneOffset()+'&wifi='+(wifiOff?'off':'on')+'&iv='+iv+(solo?'&solo=1':'')).then(function(ok){
  if(!ok)return;
  if(wifiOff){alert('記録を始めました。基板は再起動して、Wi-Fi をオフにして記録を続けます（LED5 が赤く点滅します）。'
   +(solo?'役割は「単体」に切り替えました。':'')
   +'止めるときは、BOOT ボタンを長押しして設定モードに入り、このページの「■ 停止」を押してください。');return;}
  // 基板が記録を始めたかを、少し待ってから確かめる
  setTimeout(function(){if(!rec.on)alert('記録が始まっていないようです。もう一度「● 記録」を押してください。');},3000);
 });
}
document.getElementById('recon').onclick=function(){startRec(false);};
document.getElementById('recoff').onclick=function(){startRec(true);};
document.getElementById('reccancel').onclick=function(){dlg.className='';};
// CSV は、基板からダウンロードする。ファイル名は、押した日時
// iPhone の Wi-Fi のログイン画面 (Safari ではない画面) は、ファイルを保存できない。
// Safari の User-Agent には "Safari/" が入るが、ログイン画面には入らないので、それで見分ける
var ua=navigator.userAgent;
var isIOS=/iPhone|iPad|iPod/.test(ua)||(navigator.platform==='MacIntel'&&navigator.maxTouchPoints>1);
var isIOSCaptive=isIOS&&!/Safari\//.test(ua);
var iosdlg=document.getElementById('iosdlg'),pageUrl='http://'+location.host+'/';
document.getElementById('iosurl').textContent=pageUrl;
document.getElementById('iosclose').onclick=function(){iosdlg.className='';};
// Wi-Fi を切る: 設定モードを終了して、通常モード (Wi-Fi オフ) に戻る (portal.cpp の /exit)
document.getElementById('wifioff').onclick=function(){
 if(!confirm('基板の Wi-Fi を切って、通常モードに戻ります。このページは使えなくなります。'
  +(rec.on?'記録は、そのまま続きます。':'')+'よろしいですか？'))return;
 fetch('/exit',{method:'POST'}).catch(function(){}).then(function(){
  document.getElementById('st').textContent='Wi-Fi を切りました。もう一度使うときは、BOOT ボタンを長押ししてください。';});
};
// 接続を完了する: 基板が、この iPhone のネットの確認に「つながっている」と答えるようにする (portal.cpp)
document.getElementById('iosauth').onclick=function(){location.href='/auth';};
document.getElementById('ioscopy').onclick=function(){
 var t=document.createElement('textarea');t.value=pageUrl;document.body.appendChild(t);t.select();
 var ok=false;try{ok=document.execCommand('copy');}catch(e){}t.remove();
 this.textContent=ok?'コピーしました':'コピーできませんでした';};
// ---- CSV を作る (/log.bin から。devlog.cpp の nextSample、makeLine と同じ計算で、/log.csv と同じ内容にする)
// buf: /log.bin の中身 (ArrayBuffer)。progress(割合) を呼びながら少しずつ作り、Blob を返す
function binToCsv(buf,progress){return new Promise(function(resolve,reject){
 var dv=new DataView(buf),u=new Uint8Array(buf);
 if(u.length<20||String.fromCharCode(u[0],u[1],u[2],u[3])!=='PMLB'||u[4]!==1){reject(new Error('形式が違います'));return;}
 var L=u[5],headLen=dv.getUint32(8,true),lines=dv.getUint32(12,true),end=dv.getUint32(16,true);
 var B=20+headLen;                         // 記録 (フラッシュの先頭) の位置
 if(B+end>u.length||end<4096+64){reject(new Error('記録が途中で切れています'));return;}
 // ヘッダ (LogHeader) と、停止の情報 (StopInfo)
 var ver=u[B+4],ses=u[B+5],tz=dv.getInt16(B+6,true),start=Number(dv.getBigInt64(B+8,true));
 var iv=ver>=4?dv.getUint32(B+53,true):0;if(iv===0xFFFFFFFF)iv=0;
 var stopRel=dv.getUint32(B+68,true),stopPhone=Number(dv.getBigInt64(B+72,true));
 var corr=u[B+64]===0&&stopPhone>0&&stopRel>0,err=corr?stopPhone-(start+stopRel):0;
 // HPF (high_pass_filter.cpp と同じ。値は 2^53 より十分小さいので、Number で正確に計算できる)
 var hp={px:Number(dv.getBigInt64(B+16,true)),y:Number(dv.getBigInt64(B+24,true)),init:u[B+32]!==0};
 function rsa(v,s){var p=Math.pow(2,s),r=Math.floor((Math.abs(v)+p/2)/p);return v<0?-r:r;}
 function hpf(x,sh){if(sh<1)sh=1;if(sh>30)sh=30;var cx=x*256;
  if(!hp.init){hp.px=cx;hp.y=0;hp.init=true;}
  var dec=rsa(hp.y,sh);hp.y=hp.y+(cx-hp.px)-dec;hp.px=cx;return rsa(hp.y,8);}
 function u24(o){return u[o]|(u[o+1]<<8)|(u[o+2]<<16);}
 function i16(v){return (v<<16)>>16;}
 // 次のサンプル (S に入れる)。終わりなら false
 var S={off:4096,inF:false,n:0,idx:0,kind:0,pos:0,pe:0,rel:0,raw:0,sh:8,t:0,ah:0,avgHave:false,bat:0,r0:0,per:0};
 function next(){
  while(!S.inF||S.idx>S.n){
   S.inF=false;
   if(S.off+6>end)return false;
   var o=B+S.off,tag=u[o];
   if(u[o+1]!==ses)return false;
   if(tag===0xB0){hp.px=0;hp.y=0;hp.init=false;S.off+=6;continue;}          // 再起動
   if(tag===0xBA){S.bat=dv.getUint16(o+2,true);S.off+=4;continue;}           // 電池電圧
   if(tag===0xA7){                                                           // 平均
    if(S.off+14>end)return false;
    S.rel=dv.getUint32(o+2,true);S.raw=u24(o+6);S.t=dv.getInt16(o+9,true);S.ah=(u24(o+11)<<8)>>8;
    S.kind=2;S.avgHave=true;S.n=0;S.idx=0;S.inF=true;S.off+=14;continue;}
   if(tag===0xA8){                                                           // 平均 (前との差)
    if(!S.avgHave||S.off+8>end)return false;
    S.rel=(S.rel+dv.getInt8(o+2)+iv)>>>0;S.raw+=dv.getInt16(o+3,true);S.t=i16(S.t+dv.getInt8(o+5));
    S.ah=dv.getInt16(o+6,true);S.kind=2;S.n=0;S.idx=0;S.inF=true;S.off+=8;continue;}
   if(tag===0xA9){                                                           // 毎サンプル (版 6。センサの間隔)
    if(S.off+17>end)return false;
    var fl=dv.getUint16(o+13,true);S.n=u[o+2];
    if(S.n>239||fl>733||S.off+17+fl>end)return false;
    S.kind=3;S.pos=o+17;S.pe=o+17+fl;S.sh=u[o+3];S.r0=dv.getUint32(o+4,true);S.per=dv.getUint16(o+15,true);
    S.rel=S.r0;S.raw=u24(o+8);S.t=dv.getInt16(o+11,true);S.idx=0;S.inF=true;S.off+=17+fl;continue;}
   if(tag===0xA6){                                                           // 毎サンプル (版 5)
    if(S.off+15>end)return false;
    var bl=dv.getUint16(o+13,true);S.n=u[o+2];
    if(S.n>119||bl>484||S.off+15+bl>end)return false;
    S.kind=1;S.pos=o+15;S.pe=o+15+bl;S.sh=u[o+3];S.rel=dv.getUint32(o+4,true);S.raw=u24(o+8);
    S.t=dv.getInt16(o+11,true);S.idx=0;S.inF=true;S.off+=15+bl;continue;}
   if(tag!==0xA5)return false;                                               // 毎サンプル (版 4 まで)
   S.n=u[o+2];if(S.n>29)return false;
   var len=13+S.n*3;if(S.off+len>end)return false;
   S.kind=0;S.pos=o+13;S.sh=u[o+3];S.rel=dv.getUint32(o+4,true);S.raw=u24(o+8);
   S.t=dv.getInt16(o+11,true);S.idx=0;S.inF=true;S.off+=len;
  }
  if(S.idx>0&&S.kind===3){                  // 符号を 1 つ読む (devlog.cpp の unfifoCode)
   var q=S.pos,qe=S.pe,c,dd;
   while(qe-q>=2&&u[q]===0xC1){S.t=i16(S.t+dv.getInt8(q+1));q+=2;}
   if(q>=qe)return false;
   c=u[q];
   if(c<0x80){dd=c&0x40?c-128:c;q+=1;}
   else if(c<0xC0){if(qe-q<2)return false;var w=((c&0x3F)<<8)|u[q+1];dd=w>=8192?w-16384:w;q+=2;}
   else if(c===0xC0){if(qe-q<3)return false;dd=dv.getInt16(q+1,true);q+=3;}
   else return false;
   // 時刻は、先頭の時刻と間隔から (devlog.cpp の fifoRel)
   S.pos=q;S.rel=(S.r0+Math.floor((S.idx*S.per+500)/1000))>>>0;S.raw+=dd;
  }else if(S.idx>0&&S.kind===1){            // 符号を 1 つ読む (devlog.cpp の unpackSample)
   var p=S.pos,e=S.pe,b,dt,d;
   while(e-p>=2&&u[p]===0xC1){S.t=i16(S.t+dv.getInt8(p+1));p+=2;}
   if(p>=e)return false;
   b=u[p];
   if(b<0x80){dt=33;d=b&0x40?b-128:b;p+=1;}
   else if(b<0xC0){if(e-p<2)return false;var v=((b&7)<<8)|u[p+1];dt=32+((b>>3)&7);d=v>=1024?v-2048:v;p+=2;}
   else if(b===0xC0){if(e-p<4)return false;dt=u[p+1];d=dv.getInt16(p+2,true);p+=4;}
   else return false;
   S.pos=p;S.rel=(S.rel+dt)>>>0;S.raw+=d;
  }else if(S.idx>0){S.rel=(S.rel+u[S.pos])>>>0;S.raw+=dv.getInt16(S.pos+1,true);S.pos+=3;}
  S.idx++;return true;
 }
 // C の %0Nd (幅は符号を含む)
 function z(v,w){var s=''+Math.abs(v);while(s.length<w-(v<0?1:0))s='0'+s;return (v<0?'-':'')+s;}
 function clamp(v,l){return v>l?l:(v<-l?-l:v);}
 var BLANK=new Array(L-1).join(' ')+'\r\n',cs=null,ds='';
 function line(){
  if(!next())return BLANK;
  var h=S.kind===2?S.ah:hpf(S.raw,S.sh);
  var unix=start+S.rel;
  // 時刻の補正 (err*rel は 2^53 より小さく、割った値の誤差は 1 より十分小さいので、切り捨ては C と同じ)
  if(corr)unix+=Math.trunc(err*S.rel/stopRel);
  var loc=unix-tz*60000,sec=Math.trunc(loc/1000);
  if(sec!==cs){cs=sec;var t=new Date(sec*1000);
   ds=z(t.getUTCFullYear(),4)+'-'+z(t.getUTCMonth()+1,2)+'-'+z(t.getUTCDate(),2)+' '
    +z(t.getUTCHours(),2)+':'+z(t.getUTCMinutes(),2)+':'+z(t.getUTCSeconds(),2);}
  var pk=Math.trunc((S.raw*25+8)/16),hc=clamp(h,9999999),tc=clamp(S.t,9999),ta=Math.abs(tc);
  var s=ds+'.'+z(loc%1000,3)+','+z(unix,13)+','+z(S.rel>99999999?99999999:S.rel,8)+','
   +z(Math.trunc(pk/10000)%10000,4)+'.'+z(pk%10000,4)+','+z(S.raw,8)+','
   +(hc<0?'-':'+')+z(Math.abs(hc),7)+','+(tc<0?'-':'+')+z(Math.trunc(ta/100),2)+'.'+z(ta%100,2)+',';
  if(ver>=3&&s.length<L+8)s+=z(S.bat>9999?9999:S.bat,4);   // 版 2 は空欄
  if(s.length>L-2)s=s.substring(0,L-2);
  while(s.length<L-2)s+=' ';
  return s+'\r\n';
 }
 var parts=[u.subarray(20,B)],done=0,CH=20000;
 (function step(){
  try{
   var a=[],n=Math.min(CH,lines-done);
   for(var i=0;i<n;i++)a.push(line());
   done+=n;if(a.length)parts.push(new Blob([a.join('')]));
   if(progress)progress(lines?done/lines:1);
   if(done<lines){setTimeout(step,0);return;}
   resolve(new Blob(parts,{type:'text/csv'}));
  }catch(e){reject(e);}
 })();
});}
// ---- ここまで (CSV を作る)
// CSV の保存。ふだんのブラウザ (Chrome、Safari など) では、記録をそのまま受け取り (/log.bin)、
// このページで CSV を作る (速い)。Wi-Fi のログイン画面は、ページで作ったファイルを保存できないので、
// 基板が作る CSV (/log/<ファイル名>) を保存する (遅い)。Android のログイン画面は WebView で、
// User-Agent に "; wv)" が入るので、それで見分ける (iPhone のログイン画面は、上の isIOSCaptive)
var canFast=!/; wv\)/.test(ua)&&!!(window.fetch&&window.Blob&&window.URL&&URL.createObjectURL&&window.BigInt
 &&DataView.prototype.getBigInt64);
var fast={busy:false,text:'',at:0};
function setFast(t){fast.text=t;fast.at=Date.now();recUi();}
function csvName(){var d=new Date();
 return 'pressure-monitor_'+(devName?devName.replace(/[\\\/:*?"<>|\s]/g,'_')+'_':'')
  +d.getFullYear()+pad(d.getMonth()+1,2)+pad(d.getDate(),2)+'-'
  +pad(d.getHours(),2)+pad(d.getMinutes(),2)+pad(d.getSeconds(),2)+'.csv';}
function saveLink(href,fn){
 var a=document.createElement('a');a.href=href;
 // download を付けると、iPhone の Safari では、表として開かずに、ダウンロードの確認が出る
 a.setAttribute('download',fn);document.body.appendChild(a);a.click();a.remove();}
function saveFast(fn){
 fast.busy=true;setFast('記録を受信しています...');
 fetch('/log.bin',{cache:'no-store'}).then(function(r){
  if(!r.ok)throw new Error('エラー '+r.status);
  var total=+r.headers.get('Content-Length')||0;
  if(!r.body||!r.body.getReader)return r.arrayBuffer();
  var rd=r.body.getReader(),ch=[],got=0;
  function pump(){return rd.read().then(function(x){
   if(x.done){var b=new Uint8Array(got),o=0;for(var i=0;i<ch.length;i++){b.set(ch[i],o);o+=ch[i].length;}return b.buffer;}
   ch.push(x.value);got+=x.value.length;
   setFast('記録を受信中 '+mb(got)+(total?' / '+mb(total)+' MB（'+Math.floor(100*got/total)+'%）':' MB'));
   return pump();});}
  return pump();
 }).then(function(buf){
  return binToCsv(buf,function(f){setFast('CSV を作成中（'+Math.floor(100*f)+'%）');});
 }).then(function(blob){
  var url=URL.createObjectURL(blob);saveLink(url,fn);
  setTimeout(function(){URL.revokeObjectURL(url);},60000);
  fast.busy=false;setFast('CSV を作りました（'+mb(blob.size)+' MB）');
 }).catch(function(e){
  fast.busy=false;setFast('');
  if(confirm('記録を受け取れませんでした（'+e.message+'）。基板が CSV を作る方法（時間がかかります）で保存しますか？'))
   saveSlow(fn);
 });
}
function saveSlow(fn){saveLink('/log/'+encodeURIComponent(fn),fn);}
dlA.onclick=function(){
 if(rec.count===0){alert('記録がありません。');return false;}
 if(isIOSCaptive){iosdlg.className='on';return false;}
 if(canFast){if(!fast.busy)saveFast(csvName());return false;}
 // 基板が CSV を作る。大きいときは、時間がかかることを先に知らせる (1 行 88 バイト、約 140KB/秒)。
 // Android のログイン画面では、Chrome で開き直すと速く保存できることを案内する。Android は、
 // インターネットにつながらない Wi-Fi ではなく、モバイルデータ通信で 192.168.4.1 を開こうとするので、
 // モバイルデータ通信をオフにしてもらう
 var size=rec.count*88,min=Math.max(1,Math.round(size/140000/60));
 if(size>5*1048576){
  var msg=/; wv\)/.test(ua)
   ?'CSV は約 '+mb(size)+' MB で、この画面では保存に約 '+min+' 分かかります。\n\n'
    +'速く保存するには（数秒〜数十秒）:\n'
    +'1. 「キャンセル」を押して、この画面を閉じる（Wi-Fi はつないだまま）\n'
    +'2. スマホのモバイルデータ通信をオフにする\n'
    +'3. Chrome で '+pageUrl+' を開き、「CSV 保存」を押す\n'
    +'4. 保存が終わったら、モバイルデータ通信をオンに戻す\n\n'
    +'この画面のまま保存するときは「OK」を押してください。保存が終わるまで、この画面を閉じないでください。'
   :'CSV は約 '+mb(size)+' MB です。保存先を選んだあと、約 '+min+' 分かかります。'
    +'進み具合は、この画面の下と、スマホの通知に表示されます。保存が終わるまで、このページを閉じないでください。';
  if(!confirm(msg))return false;
 }
 // ログイン画面では、押したリンクのまま移動する (ログイン画面が、そのアドレスを保存する)
 var fn=csvName();
 dlA.href='/log/'+encodeURIComponent(fn);
 dlA.setAttribute('download',fn);
 return true;
};
document.getElementById('clr').onclick=function(){
 if(rec.count===0&&!rec.on)return;
 if(confirm('基板の中の記録を全て消去します。よろしいですか？'))post('op=clear');
};
setInterval(recUi,500);

connect();lock();
</script></body></html>
)HTML";

const char AUTH_DONE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>接続を完了しました</title>
<style>body{font-family:sans-serif;margin:16px;line-height:1.6}b.u{font-size:1.1em}
button{display:block;width:100%;margin-top:10px;padding:10px;font-size:1em}</style></head><body>
<h3>接続を完了しました</h3>
<ol style="padding-left:1.3em">
 <li>右上の「<b>完了</b>」をタップして、この画面を閉じてください（Wi-Fi はつながったままになります）。
  「完了」が出ないときは、数秒待つか、このページを下に引いて読み込み直してください</li>
 <li>Chrome や Safari などのブラウザで <b class="u" id="u"></b> を開いてください</li>
 <li>「CSV 保存」をタップすると、「ファイル」アプリに保存できます</li>
</ol>
<button type="button" id="c">アドレスをコピー</button>
<p><small>保存が終わったら、Wi-Fi をほかのネットワークに切り替えてください。この Wi-Fi はインターネットに
つながらないため、つないでいる間は、ほかのアプリの通信ができません。</small></p>
<script>
var u='http://'+location.host+'/';document.getElementById('u').textContent=u;
document.getElementById('c').onclick=function(){var t=document.createElement('textarea');t.value=u;
 document.body.appendChild(t);t.select();var ok=false;try{ok=document.execCommand('copy');}catch(e){}t.remove();
 this.textContent=ok?'コピーしました':'コピーできませんでした';};
</script></body></html>)HTML";
