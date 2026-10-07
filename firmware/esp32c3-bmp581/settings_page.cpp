#include "settings_page.h"

#include "config.h"
#include "settings.h"

String buildSettingsPage(bool saved)
{
    String h;
    h.reserve(5000);
    h += F(R"HTML(<!DOCTYPE html><html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>pressure-monitor 設定</title>
<style>
body{font-family:sans-serif;margin:16px;max-width:560px}
h1{font-size:1.25em}
label{display:block;margin-top:16px;font-weight:bold}
input,select{font-size:1.1em;width:100%;box-sizing:border-box;padding:8px}
small{color:#555;display:block;margin-top:2px}
button{font-size:1.1em;padding:12px 16px;margin:16px 8px 0 0}
.box{background:#eef;padding:8px 12px;border-radius:6px;line-height:1.6}
.ok{background:#dfd;padding:8px 12px;border-radius:6px;margin-bottom:8px}
</style></head><body>
<h1>pressure-monitor 設定</h1>
)HTML");

    if (saved) h += F("<div class=\"ok\">保存しました</div>");

    h += F(R"HTML(<div class="box" id="st">読み込み中...</div>
<script>
function upd(){fetch('/status').then(function(r){return r.json();}).then(function(j){
 var s='電池: '+(j.vbat_mv/1000).toFixed(3)+' V (最低 '+(j.vbat_min_mv/1000).toFixed(3)+' V)<br>'
 +'気圧: '+j.hpa.toFixed(2)+' hPa / 温度: '+j.temp_c.toFixed(1)+' ℃<br>'
 +'接続中の端末: '+j.stations+' 台 / 空きメモリ: '+j.heap+' B<br>'
 +'前回の再起動理由: '+j.reset+' / ブラウンアウト: '+j.brownouts+' 回';
 document.getElementById('st').innerHTML=s;}).catch(function(){});}
setInterval(upd,1000);upd();
</script>
<form method="POST" action="/save">
)HTML");

    h += F("<label>不感帯 (この変化量未満は消灯)</label>"
           "<input type=\"number\" name=\"limit\" min=\"1\" max=\"5000\" value=\"");
    h += String(g_set.deltaLimit);
    h += F("\"><small>単位は 1/64 Pa。50 ≒ 0.78 Pa ≒ 高さ 6.6 cm</small>");

    h += F("<label>感度の上限 (この変化量で白)</label>"
           "<input type=\"number\" name=\"range\" min=\"4\" max=\"100000\" value=\"");
    h += String(g_set.deltaRange);
    h += F("\"><small>単位は 1/64 Pa。400 ≒ 6.25 Pa ≒ 高さ 53 cm。広い範囲なら 15000 ≒ 20 m</small>");

    h += F("<label>フィルタの時定数 (大きいほど、ゆっくりした変化まで拾う)</label>"
           "<input type=\"number\" name=\"hpf\" min=\"4\" max=\"12\" value=\"");
    h += String(g_set.hpfShift);
    h += F("\"><small>約 2 の n 乗サンプル。8 なら約 8.5 秒 (30Hz)</small>");

    h += F("<label>履歴を流す間隔 (フレーム数)</label>"
           "<input type=\"number\" name=\"trail\" min=\"1\" max=\"60\" value=\"");
    h += String(g_set.trailStepTicks);
    h += F("\"><small>1 なら毎フレーム。大きくすると LED2〜5 に長い時間の履歴が流れます</small>");

    h += F("<label>起動時の表示</label><select name=\"mode\">");
    static const char *modeNames[3] = { "気圧の変化", "気圧バー", "消灯" };
    for (int m = 0; m < 3; m++) {
        h += F("<option value=\"");
        h += String(m);
        h += F("\"");
        if (g_set.defaultMode == m) h += F(" selected");
        h += F(">");
        h += modeNames[m];
        h += F("</option>");
    }
    h += F("</select>");

    h += F("<label>LED の電流の上限 (mA)</label>"
           "<input type=\"number\" name=\"maxma\" min=\"50\" max=\"500\" value=\"");
    h += String(g_set.maxMilliamps);
    h += F("\"><small>白など明るい色で電流が増えるときの上限。下げると暗くなります</small>");

    h += F(R"HTML(<button type="submit" name="exit" value="0">保存</button>
<button type="submit" name="exit" value="1">保存して終了 (Wi-Fi をオフ)</button>
</form>
<form method="POST" action="/reset"><button type="submit">初期値に戻す</button></form>
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
    j.reserve(256);
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
    j += F("}");
    return j;
}
