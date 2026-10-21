#include "portal.h"

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>

#include "config.h"
#include "settings.h"
#include "settings_page.h"
#include "monitor_page.h"
#include "devlog.h"
#include "display.h"
#include "battery.h"
#include "boot.h"
#include "modes.h"
#include "link.h"

static AsyncWebServer s_server(80);
static DNSServer      s_dns;
static String         s_portalUrl;         // 例: http://192.168.4.1/

// 設定ページの最後の操作の時刻。WebSocket / HTTP の受信タスクが更新し、loop() が読むので volatile
static volatile uint32_t s_lastActivityMs = 0;
static bool     s_restartPending = false;
static uint32_t s_restartAtMs    = 0;

static uint32_t s_vbatNowMv  = 0;          // 電池電圧 (1 秒ごと)
static uint32_t s_vbatMinMv  = 0xFFFFFFFFUL;
static uint32_t s_lastVbatMs = 0;

static float    s_hPa   = 0.0f;
static float    s_tempC = 0.0f;

static bool     s_started = false;         // portalBegin() を呼んだ (設定モード)

// 表示ページ (monitor_page.h) へ送るサンプル。約 100ms ごとに、まとめて WebSocket で送る
static const uint8_t  PUSH_MAX_SAMPLES = 16;
static const uint32_t PUSH_INTERVAL_MS = 100;
static const size_t   PUSH_HEADER = 24;
static uint8_t  s_push[PUSH_HEADER + 16 * PUSH_MAX_SAMPLES];
static uint8_t  s_pushCount  = 0;
static uint32_t s_lastPushMs = 0;

static AsyncWebSocket s_ws("/ws");

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// たまったサンプルを、1 つのフレームにして、全ての接続先へ送る (形式は monitor_page.h)
static void flushSamples(uint32_t nowMs)
{
    s_lastPushMs = nowMs;
    if (s_pushCount == 0) return;
    const int16_t t100 = (int16_t)lroundf(s_tempC * 100.0f);
    s_push[0] = 'M';
    s_push[1] = 3;
    s_push[2] = s_pushCount;
    s_push[3] = (uint8_t)g_set.mode;
    s_push[4] = (uint8_t)t100;
    s_push[5] = (uint8_t)((uint16_t)t100 >> 8);
    // 基板の中への記録の状態 (devlog.h)
    s_push[6] = (devlogRecording() ? 1 : 0) | (devlogFull() ? 2 : 0) | (devlogAvailable() ? 0 : 4);
    s_push[7] = (uint8_t)(devlogIntervalMs() / 1000);   // 記録の間隔 [秒]。0 = 毎サンプル (30Hz)
    put32(s_push + 8, devlogCount());
    put32(s_push + 12, devlogCapacity());
    // CSV のダウンロードの進み具合 (送ったバイト数と、全体のバイト数)
    uint32_t dlSent = 0, dlTotal = 0;
    devlogDownloadProgress(dlSent, dlTotal);
    put32(s_push + 16, dlSent);
    put32(s_push + 20, dlTotal);
    // 送信が詰まっている接続先があるときは、このフレームを捨てる (メモリを使い切らないため)
    if (s_ws.count() > 0 && s_ws.availableForWriteAll()) {
        s_ws.binaryAll(s_push, PUSH_HEADER + 16 * s_pushCount);
    }
    s_pushCount = 0;
}

// ---------------------------------------------------------------- handlers

// 最初に開くページ: 気圧の表示 (monitor_page.h)
static void handleRoot(AsyncWebServerRequest *req)
{
    s_lastActivityMs = millis();
    req->send(200, "text/html; charset=utf-8", (const uint8_t *)MONITOR_PAGE_HTML, strlen(MONITOR_PAGE_HTML));
}

// 基板の中への記録の操作 (表示ページの「記録」「停止」「消去」)。op = start / stop / clear。
// start では、スマホの時計 (t、UNIX 時刻 [ms])、タイムゾーン (tz、分)、wifi (off なら、記録を始めたあと
// 再起動して Wi-Fi をオフにする) を受け取る。stop では、スマホの時計 (t) を受け取り、時刻を補正する
static void handleRec(AsyncWebServerRequest *req)
{
    s_lastActivityMs = millis();
    if (!devlogAvailable()) {
        req->send(503, "text/plain; charset=utf-8",
                  "記録の領域（パーティション spiffs）がありません。ファームウェアを書き込み直してください。");
        return;
    }
    const String op = req->hasParam("op", true) ? req->getParam("op", true)->value() : String();
    if (op == "start") {
        const int64_t t  = req->hasParam("t", true) ? strtoll(req->getParam("t", true)->value().c_str(), nullptr, 10) : 0;
        const int32_t tz = req->hasParam("tz", true) ? (int32_t)req->getParam("tz", true)->value().toInt() : 0;
        const bool wifiOff = req->hasParam("wifi", true) && req->getParam("wifi", true)->value() == "off";
        // 役割が送信・ハブのときは、Wi-Fi をオフにしても ESP-NOW が動き続け、電池の消費が大きい。
        // ページで「単体に切り替える」を選ぶと、solo=1 が届くので、記録を始める前に単体にする
        // (通常モードで再起動したときから有効になる)
        if (req->hasParam("solo", true) && req->getParam("solo", true)->value() == "1" &&
            g_set.role != LINK_ROLE_STANDALONE) {
            g_set.role = LINK_ROLE_STANDALONE;
            settingsSaveRole();
            Serial.println(F("# role -> standalone (before recording)"));
        }
        // 記録の間隔 (iv、ms)。0 = 毎サンプル (30Hz)、1000 = 1 秒、30000 = 30 秒。ほかの値は 0 にする
        uint32_t iv = req->hasParam("iv", true) ? (uint32_t)req->getParam("iv", true)->value().toInt() : 0;
        if (iv != 1000 && iv != 30000) iv = 0;
        devlogRequestStart(t, tz, wifiOff, iv);
    } else if (op == "stop") {
        // 停止したときのスマホの時計で、記録の時刻のずれを補正する
        const int64_t t = req->hasParam("t", true) ? strtoll(req->getParam("t", true)->value().c_str(), nullptr, 10) : 0;
        devlogRequestStop(t);
    } else if (op == "clear") {
        devlogRequestClear();
    } else {
        req->send(400, "text/plain", "bad op");
        return;
    }
    req->send(204);
}

// 設定のページ (表示ページの「設定」ボタンから開く)
static void handleSettings(AsyncWebServerRequest *req)
{
    s_lastActivityMs = millis();
    req->send(200, "text/html; charset=utf-8", buildSettingsPage(req->hasParam("saved")));
}

// 状況の確認用 (JSON)。ページを開いているだけで「操作あり」とはみなさないよう、
// s_lastActivityMs は更新しない
static void handleStatus(AsyncWebServerRequest *req)
{
    DeviceStatus st;
    st.vbatNowMv  = s_vbatNowMv;
    st.vbatMinMv  = (s_vbatMinMv == 0xFFFFFFFFUL) ? 0 : s_vbatMinMv;
    st.hPa        = s_hPa;
    st.tempC      = s_tempC;
    st.stations   = WiFi.softAPgetStationNum();
    st.freeHeap   = ESP.getFreeHeap();
    st.resetReason = bootResetReasonName();
    st.brownouts  = settingsBrownoutCount();
    st.modeNumber = (uint8_t)(g_set.mode + 1);
    st.modeName   = modeName((uint8_t)g_set.mode);
    st.roleName   = linkRoleName((uint8_t)g_set.role);
    st.nodeName   = settingsNodeName();
    req->send(200, "application/json", buildStatusJson(st));
}

// POST のフォームの値を読む
static void postInt(AsyncWebServerRequest *req, const char *name, int32_t &out)
{
    if (req->hasParam(name, true)) out = (int32_t)req->getParam(name, true)->value().toInt();
}

static void handleSave(AsyncWebServerRequest *req)
{
    s_lastActivityMs = millis();

    postInt(req, "limit", g_set.deltaLimit);
    postInt(req, "range", g_set.deltaRange);
    postInt(req, "hpf",   g_set.hpfShift);
    postInt(req, "trail", g_set.trailStepTicks);
    postInt(req, "bright", g_set.brightness);
    postInt(req, "hue",   g_set.hue);
    postInt(req, "sat",   g_set.sat);
    postInt(req, "val",   g_set.val);
    postInt(req, "vmod",  g_set.valMod);
    postInt(req, "role",  g_set.role);
    postInt(req, "group", g_set.group);
    postInt(req, "hubch", g_set.hubChannel);

    // 装置の名前。空欄なら、初期値 (ps-XXXXXX) を使う
    if (req->hasParam("nname", true)) {
        String name = req->getParam("nname", true)->value();
        name.trim();
        if (name == settingsDefaultNodeName()) name = "";   // 初期値と同じなら、保存しない
        settingsCopyName(g_set.nodeName, name.c_str(), NODE_NAME_MAX);
    }

    // ハブの接続先の Wi-Fi。パスワードは、空欄なら今のものを残す
    if (req->hasParam("wssid", true)) {
        String ssid = req->getParam("wssid", true)->value();
        ssid.trim();
        strlcpy(g_set.wifiSsid, ssid.c_str(), sizeof(g_set.wifiSsid));
    }
    if (req->hasParam("wpass", true)) {
        const String pass = req->getParam("wpass", true)->value();
        if (pass.length() > 0) strlcpy(g_set.wifiPass, pass.c_str(), sizeof(g_set.wifiPass));
    }
    if (req->hasParam("wpclr", true) && req->getParam("wpclr", true)->value() == "1") {
        g_set.wifiPass[0] = '\0';
    }

    settingsClamp();
    settingsSave();
    Serial.printf("# settings saved: limit=%ld range=%ld hpf=%ld trail=%ld bright=%ld hue=%ld sat=%ld val=%ld vmod=%ld"
                  " role=%s group=%ld hubch=%ld name=%s wifi=\"%s\"%s\n",
                  (long)g_set.deltaLimit, (long)g_set.deltaRange, (long)g_set.hpfShift,
                  (long)g_set.trailStepTicks, (long)g_set.brightness,
                  (long)g_set.hue, (long)g_set.sat, (long)g_set.val, (long)g_set.valMod,
                  linkRoleName((uint8_t)g_set.role), (long)g_set.group, (long)g_set.hubChannel,
                  settingsNodeName().c_str(),
                  g_set.wifiSsid, g_set.wifiPass[0] ? " (password set)" : "");

    bool exitAfter = req->hasParam("exit", true) &&
                     req->getParam("exit", true)->value() == "1";
    if (exitAfter) {
        Serial.println(F("# settings page: save and exit"));
        req->send(200, "text/html; charset=utf-8",
            "<!DOCTYPE html><html lang=\"ja\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"></head>"
            "<body style=\"font-family:sans-serif;margin:16px\">"
            "<p>保存しました。Wi-Fi をオフにして、通常モードに戻ります。</p></body></html>");
        s_restartPending = true;
        s_restartAtMs    = millis() + 800;     // 応答を送りきってから再起動する
        return;
    }

    req->redirect("/settings?saved=1");
}

// ---- 設定ページの操作中の、LED へのリアルタイムの反映 (保存はしない)
// 保存するのは「保存」を押したときだけで、保存しないまま再起動すると、元の値に戻る。
// 表示モードの切り替えだけは、押したときに保存する (BOOT ボタンと同じ)。
//
// 値は、WebSocket (/ws) で受け取る。スライドバーを動かしている間は、1 秒に 30 回ほど届くため、
// HTTP の POST (/live) のように、応答を待つ往復の時間がかからない。
// WebSocket が使えないときは、HTTP の POST (/live) で受け取る (ページのスクリプトが切り替える)

// 名前と値を 1 組、設定に反映する。ページの表示モードの番号は 1〜4、内部は 0〜3
static void setLiveValue(const String &key, int32_t v)
{
    if      (key == "bright") g_set.brightness     = v;
    else if (key == "limit")  g_set.deltaLimit     = v;
    else if (key == "range")  g_set.deltaRange     = v;
    else if (key == "hpf")    g_set.hpfShift       = v;
    else if (key == "trail")  g_set.trailStepTicks = v;
    else if (key == "hue")    g_set.hue            = v;
    else if (key == "sat")    g_set.sat            = v;
    else if (key == "val")    g_set.val            = v;
    else if (key == "vmod")   g_set.valMod         = v;
    else if (key == "mode")   g_set.mode           = v - 1;
}

// 反映のあとの後始末 (範囲を整える。モードが変わったら保存する)
static void finishLive(int32_t oldMode)
{
    s_lastActivityMs = millis();
    settingsClamp();
    if (g_set.mode != oldMode) {
        settingsSaveMode();
        Serial.printf("# mode -> %ld (%s) [settings page]\n", (long)g_set.mode + 1,
                      modeName((uint8_t)g_set.mode));
    }
}

// HTTP の POST (/live)
static void handleLive(AsyncWebServerRequest *req)
{
    const int32_t oldMode = g_set.mode;
    for (size_t i = 0; i < req->params(); i++) {
        const AsyncWebParameter *p = req->getParam(i);
        if (p->isPost()) setLiveValue(p->name(), (int32_t)p->value().toInt());
    }
    finishLive(oldMode);
    req->send(204);
}

// WebSocket のメッセージ。"bright=255&limit=50&..." の形式
static void onWsMessage(const uint8_t *data, size_t len)
{
    String s;
    s.reserve(len);
    for (size_t i = 0; i < len; i++) s += (char)data[i];

    const int32_t oldMode = g_set.mode;
    int pos = 0;
    const int n = (int)s.length();
    while (pos < n) {
        int amp = s.indexOf('&', pos);
        if (amp < 0) amp = n;
        int eq = s.indexOf('=', pos);
        if (eq > pos && eq < amp) {
            setLiveValue(s.substring(pos, eq), (int32_t)s.substring(eq + 1, amp).toInt());
        }
        pos = amp + 1;
    }
    finishLive(oldMode);
}

static void onWsEvent(AsyncWebSocket *, AsyncWebSocketClient *, AwsEventType type,
                      void *arg, uint8_t *data, size_t len)
{
    if (type != WS_EVT_DATA) return;
    const AwsFrameInfo *info = (const AwsFrameInfo *)arg;
    // 短い 1 フレームのテキストだけを扱う
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        onWsMessage(data, len);
    }
}

static void handleReset(AsyncWebServerRequest *req)
{
    s_lastActivityMs = millis();
    settingsReset();
    Serial.println(F("# settings reset to default"));

    req->redirect("/settings?saved=1");
}

// 登録のないアドレスへのアクセス (キャプティブポータル。説明は portal.h)
static void handleNotFound(AsyncWebServerRequest *req)
{
    Serial.printf("# http (portal) %s%s\n", req->host().c_str(), req->url().c_str());

    if (PORTAL_DIRECT_HTML) {
        req->send(200, "text/html; charset=utf-8", (const uint8_t *)MONITOR_PAGE_HTML,
                  strlen(MONITOR_PAGE_HTML));
    } else {
        req->redirect(s_portalUrl);
    }
}

// ---------------------------------------------------------------- public

void portalBegin()
{
    s_started = true;
    WiFi.persistent(false);             // Wi-Fi の設定をフラッシュに書かない
    WiFi.mode(WIFI_AP);

    bool useWpa = (strlen(AP_PASSWORD) >= 8);
    bool ok = useWpa ? WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS)
                     : WiFi.softAP(AP_SSID, nullptr,     AP_CHANNEL, 0, AP_MAX_CLIENTS);
    WiFi.setTxPower(AP_TX_POWER);

    IPAddress ip = WiFi.softAPIP();
    s_portalUrl = String("http://") + ip.toString() + "/";
    s_dns.start(53, "*", ip);           // 全ての名前を基板 (192.168.4.1) に向ける

    s_server.on("/",       HTTP_GET,  handleRoot);
    s_server.on("/settings", HTTP_GET, handleSettings);
    s_server.on("/log.csv",  HTTP_GET, devlogHandleCsv);
    s_server.on("/log",      HTTP_GET, devlogHandleCsv);   // /log/<ファイル名> (装置の名前を含む)
    s_server.on("/rec",      HTTP_POST, handleRec);
    s_server.on("/status", HTTP_GET,  handleStatus);
    s_server.on("/save",   HTTP_POST, handleSave);
    s_server.on("/live",   HTTP_POST, handleLive);
    s_ws.onEvent(onWsEvent);
    s_server.addHandler(&s_ws);
    s_server.on("/reset",  HTTP_POST, handleReset);
    s_server.onNotFound(handleNotFound);
    s_server.begin();

    s_lastActivityMs = millis();

    Serial.printf("# settings mode: Wi-Fi AP %s %s\n", ok ? "started" : "FAILED",
                  useWpa ? "(WPA2)" : "(open, no password)");
    Serial.printf("#   SSID : %s\n", AP_SSID);
    if (useWpa) Serial.printf("#   password : %s\n", AP_PASSWORD);
    Serial.printf("#   URL  : %s\n", s_portalUrl.c_str());
    if (useWpa) {
        Serial.printf("#   QR (Wi-Fi): WIFI:T:WPA;S:%s;P:%s;;\n", AP_SSID, AP_PASSWORD);
    } else {
        Serial.printf("#   QR (Wi-Fi): WIFI:T:nopass;S:%s;;\n", AP_SSID);
    }
}

// 電池電圧は分圧の出力抵抗が高く、C12 (100nF) でならされるため、数 ms の短い
// 落ち込みは捉えられません。落ち込みが原因のリセットは、ブラウンアウトの回数で確認できます
void portalService(uint32_t nowMs)
{
    s_dns.processNextRequest();
    s_ws.cleanupClients();              // 切れた WebSocket の接続を片づける

    // 表示ページへ、たまったサンプルを送る
    if (s_pushCount > 0 && nowMs - s_lastPushMs >= PUSH_INTERVAL_MS) flushSamples(nowMs);

    // ページ (表示または設定) を開いている間は、無操作とみなさない。
    // ページを閉じる (スマホの画面が消える) と、WebSocket が切れて、時間を数え始める
    // 基板の中に記録している間も、終了しない (スマホの画面が消えても、記録を続ける)
    // CSV をダウンロードしている間も、終了しない (大きい記録は、数分かかる)
    if (s_ws.count() > 0 || devlogRecording() || devlogDownloading()) s_lastActivityMs = nowMs;

    // 電池電圧の監視 (1 秒ごと)。最低値を記録して、Wi-Fi 送信時の電圧の落ち込みを見る
    if (nowMs - s_lastVbatMs >= 1000) {
        s_lastVbatMs = nowMs;
        s_vbatNowMv  = batteryReadMilliVolts();
        if (s_vbatNowMv >= BATTERY_PRESENT_MV && s_vbatNowMv < s_vbatMinMv) {
            s_vbatMinMv = s_vbatNowMv;
        }
    }

    // 「保存して終了」の再起動
    if (s_restartPending && (int32_t)(nowMs - s_restartAtMs) >= 0) {
        bootRebootInto(false);
    }

    // 無操作のタイムアウト (ページを閉じてから SETTINGS_IDLE_TIMEOUT_MS)
    // 引き算は符号付きで行う。受信タスクが、nowMs を取ったあとに時刻を更新すると、
    // nowMs より新しくなる。符号なしだと、巨大な値になって、誤ってタイムアウトしてしまう
    const int32_t idleMs = (int32_t)(nowMs - s_lastActivityMs);
    if (idleMs >= (int32_t)SETTINGS_IDLE_TIMEOUT_MS) {
        Serial.println(F("# settings mode: idle timeout"));
        bootRebootInto(false);
    }
}

bool portalHasClient()
{
    return WiFi.softAPgetStationNum() > 0;
}

void portalPushSample(uint32_t ms, int32_t raw, int32_t hpf, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_started) return;
    if (s_ws.count() == 0) { s_pushCount = 0; return; }     // 見ている人がいなければ溜めない
    if (s_pushCount >= PUSH_MAX_SAMPLES) flushSamples(millis());

    uint8_t *p = s_push + PUSH_HEADER + 16 * s_pushCount;
    put32(p, ms);
    put32(p + 4, (uint32_t)raw);
    put32(p + 8, (uint32_t)hpf);
    p[12] = r; p[13] = g; p[14] = b; p[15] = 0;
    s_pushCount++;
}

void portalSetSensor(float hPa, float tempC)
{
    s_hPa   = hPa;
    s_tempC = tempC;
}
