#include "settings_page.h"

#include "config.h"
#include "settings.h"

// スライドバーと数値の入力欄を 1 組出す。
// 値は、数値の入力欄 (name = id) がフォームで送られる。スライドバーは、ページのスクリプトが
// 数値と連動させる。log = true なら、スライドバーは対数の目盛り (幅の広い値向け)
static void addField(String &h, const char *id, const char *label, int32_t minV, int32_t maxV,
                     int32_t value, const char *note, bool log = false, const char *cls = "")
{
    h += F("<label>");
    h += label;
    h += F("</label><div class=\"row\"><input type=\"range\" id=\"");
    h += id;
    h += F("_s\" class=\"");
    h += cls;
    h += F("\"><input type=\"number\" id=\"");
    h += id;
    h += F("\" name=\"");
    h += id;
    h += F("\" min=\"");
    h += String(minV);
    h += F("\" max=\"");
    h += String(maxV);
    h += F("\" value=\"");
    h += String(value);
    h += F("\" data-log=\"");
    h += log ? F("1") : F("0");
    h += F("\"></div><small>");
    h += note;
    h += F("</small>");
}

String buildSettingsPage(bool saved)
{
    String h;
    h.reserve(9000);
    h += F(R"HTML(<!DOCTYPE html><html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>pressure-monitor 設定</title>
<style>
body{font-family:sans-serif;margin:16px;max-width:560px}
h1{font-size:1.25em}
h2{font-size:1.1em;margin:28px 0 0;padding-top:12px;border-top:1px solid #ccc}
label{display:block;margin-top:16px;font-weight:bold}
input{font-size:1.1em;box-sizing:border-box;padding:8px}
.row{display:flex;gap:12px;align-items:center}
.row input[type=range]{flex:1;min-width:0;padding:0}
.row input[type=number]{width:6.5em}
small{color:#555;display:block;margin-top:2px}
button{font-size:1.1em;padding:12px 16px;margin:16px 8px 0 0}
.box{background:#eef;padding:8px 12px;border-radius:6px;line-height:1.6}
.ok{background:#dfd;padding:8px 12px;border-radius:6px;margin-bottom:8px}
#sw{height:48px;border-radius:6px;border:1px solid #888;margin-top:12px}
.mb{display:flex;gap:6px;margin:8px 0}
.mb button{flex:1;margin:0;padding:10px 4px;font-size:1em}
.mb button.on{background:#36c;color:#fff;border-color:#36c}
.hue{background:linear-gradient(to right,#f00,#ff0,#0f0,#0ff,#00f,#f0f,#f00)}
</style></head><body>
<h1>pressure-monitor 設定</h1>
)HTML");

    if (saved) h += F("<div class=\"ok\">保存しました</div>");

    h += F(R"HTML(<div class="box" id="st">読み込み中...</div>
<script>
function upd(){fetch('/status').then(function(r){return r.json();}).then(function(j){
 var s='今のモード: '+j.mode_no+' '+j.mode_name+'<br>'
 +'電池: '+(j.vbat_mv/1000).toFixed(3)+' V (最低 '+(j.vbat_min_mv/1000).toFixed(3)+' V)<br>'
 +'気圧: '+j.hpa.toFixed(2)+' hPa / 温度: '+j.temp_c.toFixed(1)+' ℃<br>'
 +'接続中の端末: '+j.stations+' 台 / 空きメモリ: '+j.heap+' B<br>'
 +'前回の再起動理由: '+j.reset+' / ブラウンアウト: '+j.brownouts+' 回';
 document.getElementById('st').innerHTML=s;mark(j.mode_no);}).catch(function(){});}
function mark(n){var b=document.querySelectorAll('.mb button');
 for(var i=0;i<b.length;i++){b[i].className=(+b[i].dataset.m===n)?'on':'';}}
setInterval(upd,1000);upd();
</script>
<h2 style="border:0;margin-top:20px">表示モード</h2>
<div class="mb">
<button type="button" data-m="1" onclick="pick(1)">1 標準</button>
<button type="button" data-m="2" onclick="pick(2)">2 ドライブ</button>
<button type="button" data-m="3" onclick="pick(3)">3 カスタム</button>
<button type="button" data-m="4" onclick="pick(4)">4 固定色</button>
</div>
<small>基板の BOOT ボタンでも切り替えられます (短く押すと、今のモードの番号の数だけ LED が青く点灯します。
その間にもう一度押すと、次のモードになります)。</small>
<p class="ok" style="background:#ffe">値を変えると、基板の LED にすぐ反映されます。反映した値は、「保存」を押すまでの間だけ有効で、
保存しないまま再起動すると、元の値に戻ります。</p>
<form method="POST" action="/save">
<h2>共通 (1 標準・2 ドライブ・3 カスタム)</h2>
)HTML");

    addField(h, "bright", "LED の明るさの最大値", 0, 255, g_set.brightness,
             "全ての色の明るさに掛かります。255 は抑えない、小さいほど暗くなります。電池の持ちも延びます。"
             "4 固定色には掛かりません");

    h += F("<h2>3 カスタム</h2><small>1 標準と 2 ドライブの感度は固定です。ここで変えられるのは、3 カスタムだけです。</small>");

    addField(h, "limit", "不感帯 (この変化量未満は消灯)", 1, 5000, g_set.deltaLimit,
             "単位は 1/64 Pa。50 ≒ 0.78 Pa ≒ 高さ 6.6 cm", true);
    addField(h, "range", "感度の上限 (この変化量で白)", 4, 100000, g_set.deltaRange,
             "単位は 1/64 Pa。400 ≒ 6.25 Pa ≒ 高さ 53 cm。広い範囲なら 15000 ≒ 20 m", true);
    addField(h, "hpf", "フィルタの時定数 (大きいほど、ゆっくりした変化まで拾う)", 4, 12, g_set.hpfShift,
             "約 2 の n 乗サンプル。8 なら約 8.5 秒 (30Hz)");
    addField(h, "trail", "履歴を流す間隔 (フレーム数)", 1, 60, g_set.trailStepTicks,
             "1 なら毎フレーム。大きくすると LED2〜5 に長い時間の履歴が流れます");

    h += F("<h2>4 固定色</h2><small>全ての LED が、同じ色で点灯します。明度を 0 にすると消灯します。</small>");

    addField(h, "hue", "色相 (0〜359 度)", 0, 359, g_set.hue, "0 赤、120 緑、240 青", false, "hue");
    addField(h, "sat", "彩度 (0〜100 %)", 0, 100, g_set.sat, "0 で白、100 で鮮やかな色");
    addField(h, "val", "明度 (0〜100 %)", 0, 100, g_set.val, "0 で消灯。電池の持ちには、小さい値が有利です");
    h += F("<div id=\"sw\"></div>");

    h += F(R"HTML(<button type="submit" name="exit" value="0">保存</button>
<button type="submit" name="exit" value="1">保存して終了 (Wi-Fi をオフ)</button>
</form>
<form method="POST" action="/reset"><button type="submit">初期値に戻す</button></form>
<script>
function hsv(h,s,v){s/=100;v/=100;var f=function(n){var k=(n+h/60)%6;return v-v*s*Math.max(0,Math.min(k,4-k,1));};
 return 'rgb('+Math.round(255*f(5))+','+Math.round(255*f(3))+','+Math.round(255*f(1))+')';}
function sw(){var g=function(i){return +document.getElementById(i).value;};
 document.getElementById('sw').style.background=hsv(g('hue'),g('sat'),g('val'));}
var ids=['bright','limit','range','hpf','trail','hue','sat','val'];
var busy=false,again=false,extra='';
function live(){
 var v=[];for(var i=0;i<ids.length;i++){var e=document.getElementById(ids[i]).value;if(e==='')return;v.push(ids[i]+'='+e);}
 if(busy){again=true;return;}
 busy=true;var b=v.join('&')+extra;extra='';
 fetch('/live',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})
 .catch(function(){}).then(function(){busy=false;if(again){again=false;live();}});}
function pick(m){extra='&mode='+m;mark(m);live();}
function link(id){var n=document.getElementById(id),s=document.getElementById(id+'_s');
 var mn=+n.min,mx=+n.max,lg=n.dataset.log==='1';
 function toS(v){return lg?Math.round(1000*Math.log(v/mn)/Math.log(mx/mn)):v;}
 function fromS(x){return lg?Math.round(mn*Math.pow(mx/mn,x/1000)):+x;}
 s.min=lg?0:mn;s.max=lg?1000:mx;s.value=toS(+n.value);
 s.oninput=function(){n.value=fromS(+s.value);sw();live();};
 n.oninput=function(){if(n.value==='')return;var v=+n.value;v=Math.max(mn,Math.min(mx,v));s.value=toS(v);sw();live();};}
ids.forEach(link);sw();
</script>
)HTML");

    h += F("<p><small>操作がないまま ");
    h += String(SETTINGS_IDLE_TIMEOUT_MS / 60000UL);
    h += F(" 分たつと、自動で終了します。基板の BOOT ボタンを長押しして終了することもできます。</small></p>"
           "</body></html>");
    return h;
}

String buildStatusJson(const DeviceStatus &s)
{
    String j;
    j.reserve(320);
    j += F("{\"vbat_mv\":");
    j += String(s.vbatNowMv);
    j += F(",\"vbat_min_mv\":");
    j += String(s.vbatMinMv);
    j += F(",\"hpa\":");
    j += String(s.hPa, 2);
    j += F(",\"temp_c\":");
    j += String(s.tempC, 1);
    j += F(",\"stations\":");
    j += String(s.stations);
    j += F(",\"heap\":");
    j += String(s.freeHeap);
    j += F(",\"reset\":\"");
    j += s.resetReason;
    j += F("\",\"brownouts\":");
    j += String(s.brownouts);
    j += F(",\"mode_no\":");
    j += String(s.modeNumber);
    j += F(",\"mode_name\":\"");
    j += s.modeName;
    j += F("\"}");
    return j;
}
