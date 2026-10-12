#include "settings_page.h"

#include "config.h"
#include "settings.h"
#include "link.h"

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

// HTML の属性値に入れる文字列 (SSID など) を、エスケープして足す
static void appendHtmlEscaped(String &h, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
            case '&':  h += F("&amp;");  break;
            case '<':  h += F("&lt;");   break;
            case '>':  h += F("&gt;");   break;
            case '"':  h += F("&quot;"); break;
            case '\'': h += F("&#39;");  break;
            default:   h += *s;          break;
        }
    }
}

// JSON の文字列に入れる値を、エスケープして足す
static void appendJsonEscaped(String &j, const char *s)
{
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') j += '\\';
        if ((uint8_t)*s >= 0x20) j += *s;
    }
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
input,select{font-size:1.1em;box-sizing:border-box;padding:8px}
select{max-width:100%}
details{margin-top:16px}
summary{font-weight:bold;cursor:pointer;padding:8px 0}
summary .chg{font-weight:normal;color:#36c;margin-left:6px}
.row{display:flex;gap:12px;align-items:center}
.row input[type=range]{flex:1;min-width:0;padding:0}
.row input[type=number]{width:6.5em}
small{color:#555;display:block;margin-top:2px}
button{font-size:1.1em;padding:12px 16px;margin:16px 8px 0 0}
.box{background:#eef;padding:8px 12px;border-radius:6px;line-height:1.6}
.ok{background:#dfd;padding:8px 12px;border-radius:6px;margin-bottom:8px}
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
function esc(s){return String(s).replace(/[&<>"']/g,function(c){return '&#'+c.charCodeAt(0)+';';});}
function upd(){fetch('/status').then(function(r){return r.json();}).then(function(j){
 var s='今のモード: '+j.mode_no+' '+j.mode_name+' / 役割: '+j.role+' ('+esc(j.node)+'、グループ '+j.group+')<br>'
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

    // 複数台の表示。リアルタイムには反映せず、保存して再起動すると有効になる
    h += F("<h2>複数台の表示 (ESP-NOW)</h2>"
           "<small>送信の装置の気圧を、ハブが集めて、スマホのブラウザに表示します。"
           "「保存して終了」で再起動すると有効になります。</small><label>役割</label><select name=\"role\" id=\"role\" onchange=\"roleUi()\">");
    static const char *const ROLE_LABELS[LINK_ROLE_COUNT] = {
        "単体 (無線はオフ。今までどおり)",
        "送信 (ハブへ気圧を送る)",
        "ハブ (集めて表示する)",
    };
    for (int i = 0; i < LINK_ROLE_COUNT; i++) {
        h += F("<option value=\"");
        h += String(i);
        h += (g_set.role == i) ? F("\" selected>") : F("\">");
        h += ROLE_LABELS[i];
        h += F("</option>");
    }
    h += F("</select><small>送信とハブは、無線を使うため、ライトスリープに入らず、電池の持ちが短くなります。"
           "ハブは USB 給電を推奨します。</small>");

    // 役割に関係する項目は、その役割を選んだときだけ表示する (ページのスクリプトが切り替える)。
    // 隠した入力欄には、ブラウザが自動でフォーカスしないので、ページを開いたときに、下の入力欄まで
    // スクロールしたり、キーボードが出たりしない。
    // 文字の入力欄は、表示していると Chrome がフォーカスしてしまうので、折り畳みの内側に入れる
    h += F("<div id=\"netopt\"><details><summary>装置の名前: ");
    appendHtmlEscaped(h, settingsNodeName().c_str());
    h += F(" <span class=\"chg\">変更する</span></summary>"
           "<label>装置の名前</label><input type=\"text\" name=\"nname\" maxlength=\"16\" "
           "autocomplete=\"off\" autocapitalize=\"off\" spellcheck=\"false\" style=\"width:100%\" value=\"");
    appendHtmlEscaped(h, settingsNodeName().c_str());
    h += F("\" placeholder=\"");
    h += settingsDefaultNodeName();
    h += F("\"><small>ハブの画面と記録で、装置を見分けるのに使います (16 バイトまで。日本語は 5 文字まで)。"
           "空欄にすると、初期値の ");
    h += settingsDefaultNodeName();
    h += F(" (ps- と MAC アドレスの下 6 桁) になります。ハブ自身の気圧も、この名前で表示します。</small></details>");

    // グループ番号 (送信・ハブ)
    h += F("<label>グループ番号 (1〜255)</label><input type=\"number\" name=\"group\" min=\"1\" max=\"255\" "
           "style=\"width:6.5em\" value=\"");
    h += String(g_set.group);
    h += F("\"><small>同じ番号の送信の装置とハブだけが、やり取りします。初期値は ");
    h += String(LINK_GROUP_DEFAULT);
    h += F(" です。近くで、本機の別のセットが動いているときは、セットごとに別の番号にしてください。"
           "番号を変えるときは、そのセットの全ての装置を、同じ番号にします。</small></div>");

    // ハブの接続先。普段は閉じておく (SSID とパスワードの組を、ブラウザがログインの欄とみなして、
    // 開いたときに、SSID の欄にフォーカスすることがあるため)。
    // パスワードは、ページに載せない (空欄のまま保存すると、今のパスワードを残す)
    h += F("<div id=\"hubopt\"><label>ハブのチャンネル</label><select name=\"hubch\">");
    for (int ch = 1; ch <= 13; ch++) {
        h += F("<option value=\"");
        h += String(ch);
        h += (g_set.hubChannel == ch) ? F("\" selected>") : F("\">");
        h += String(ch);
        if (ch == 1 || ch == 6 || ch == 11) h += F(" (おすすめ)");
        h += F("</option>");
    }
    h += F("</select><small>ハブが自分でアクセスポイントになるときのチャンネルです "
           "(Wi-Fi のルーターに接続したときは、ルーターのチャンネルになります)。"
           "互いに重ならないのは 1、6、11 です。展示会場などでは、Wi-Fi の混み具合を見るアプリで、"
           "空いているチャンネルを選んでください。送信の装置は、ハブのチャンネルを自動で探すので、"
           "変えるのはハブだけで済みます。</small>");
    const String lastIp = settingsHubLastIp();
    if (lastIp.length() > 0) {
        h += F("<small>前回、ルーターに接続したときのアドレス: http://");
        h += lastIp;
        h += F("/ (または http://");
        h += HUB_MDNS_NAME;
        h += F(".local/)</small>");
    }
    h += F("<details><summary>ハブの接続先の Wi-Fi: ");
    if (g_set.wifiSsid[0]) {
        appendHtmlEscaped(h, g_set.wifiSsid);
    } else {
        h += F("なし (ハブが自分でアクセスポイントになる)");
    }
    h += F(" <span class=\"chg\">変更する</span></summary>"
           "<label>SSID</label><input type=\"text\" name=\"wssid\" maxlength=\"32\" "
           "autocomplete=\"off\" autocapitalize=\"off\" spellcheck=\"false\" style=\"width:100%\" value=\"");
    appendHtmlEscaped(h, g_set.wifiSsid);
    h += F("\"><small>2.4GHz の Wi-Fi (WPA2 パーソナルなど) を指定します。空欄なら、ハブが自分でアクセスポイント (");
    h += HUB_SSID;
    h += F("、パスワードなし、http://192.168.4.1/) になります。接続できないときも、");
    h += String(HUB_STA_TIMEOUT_MS / 1000UL);
    h += F(" 秒後に、自分でアクセスポイントになります。送信の装置は、ハブのチャンネルを自動で探すので、"
           "設定は要りません。</small>"
           "<label>パスワード</label><input type=\"password\" name=\"wpass\" maxlength=\"64\" "
           "autocomplete=\"new-password\" style=\"width:100%\" placeholder=\"");
    h += g_set.wifiPass[0] ? F("保存済み (変えないときは空欄)") : F("未設定");
    h += F("\"><label style=\"font-weight:normal\"><input type=\"checkbox\" name=\"wpclr\" value=\"1\"> "
           "パスワードを消去する (パスワードのないネットワーク)</label></details></div>");

    h += F(R"HTML(<button type="submit" name="exit" value="0">保存</button>
<button type="submit" name="exit" value="1">保存して終了 (Wi-Fi をオフ)</button>
</form>
<form method="POST" action="/reset"><button type="submit">初期値に戻す</button></form>
<script>
var ids=['bright','limit','range','hpf','trail','hue','sat','val'];
var wsx=null,timer=null,last=0,extra='',busy=false,again=false,body='';
function openWs(){try{wsx=new WebSocket('ws://'+location.host+'/ws');
 wsx.onclose=function(){wsx=null;setTimeout(openWs,1000);};}catch(e){wsx=null;}}
function build(){var v=[];for(var i=0;i<ids.length;i++){var e=document.getElementById(ids[i]).value;if(e==='')return null;v.push(ids[i]+'='+e);}
 return v.join('&')+extra;}
function post(b){if(busy){again=true;body=b;return;}busy=true;
 fetch('/live',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})
 .catch(function(){}).then(function(){busy=false;if(again){again=false;post(body);}});}
function flush(){timer=null;
 if(wsx&&wsx.readyState===1&&wsx.bufferedAmount>0){timer=setTimeout(flush,10);return;}
 var b=build();if(b===null)return;extra='';last=Date.now();
 if(wsx&&wsx.readyState===1){wsx.send(b);}else{post(b);}}
function live(){if(timer)return;var w=33-(Date.now()-last);timer=setTimeout(flush,w>0?w:0);}
function pick(m){extra='&mode='+m;mark(m);live();}
function link(id){var n=document.getElementById(id),s=document.getElementById(id+'_s');
 var mn=+n.min,mx=+n.max,lg=n.dataset.log==='1';
 function toS(v){return lg?Math.round(1000*Math.log(v/mn)/Math.log(mx/mn)):v;}
 function fromS(x){return lg?Math.round(mn*Math.pow(mx/mn,x/1000)):+x;}
 s.min=lg?0:mn;s.max=lg?1000:mx;s.value=toS(+n.value);
 s.oninput=function(){n.value=fromS(+s.value);live();};
 n.oninput=function(){if(n.value==='')return;var v=+n.value;v=Math.max(mn,Math.min(mx,v));s.value=toS(v);live();};}
ids.forEach(link);openWs();
function roleUi(){var r=+document.getElementById('role').value;
 document.getElementById('netopt').style.display=r>0?'':'none';
 document.getElementById('hubopt').style.display=r===2?'':'none';}
roleUi();
// ページを開いたときは、上端を表示する (ブラウザが入力欄にフォーカスしても、外す)
window.addEventListener('load',function(){var a=document.activeElement;
 if(a&&(a.tagName==='INPUT'||a.tagName==='SELECT')){a.blur();window.scrollTo(0,0);}});
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
    j += F("\",\"role\":\"");
    j += s.roleName;
    j += F("\",\"node\":\"");
    appendJsonEscaped(j, s.nodeName.c_str());
    j += F("\",\"group\":");
    j += String(g_set.group);
    j += F("}");
    return j;
}
