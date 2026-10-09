#include "portal.h"

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>

#include "config.h"
#include "settings.h"
#include "settings_page.h"
#include "display.h"
#include "battery.h"
#include "boot.h"
#include "modes.h"

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

// ---------------------------------------------------------------- handlers

static void handleRoot(AsyncWebServerRequest *req)
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

    settingsClamp();
    settingsSave();
    Serial.printf("# settings saved: limit=%ld range=%ld hpf=%ld trail=%ld bright=%ld hue=%ld sat=%ld val=%ld\n",
                  (long)g_set.deltaLimit, (long)g_set.deltaRange, (long)g_set.hpfShift,
                  (long)g_set.trailStepTicks, (long)g_set.brightness,
                  (long)g_set.hue, (long)g_set.sat, (long)g_set.val);

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

    req->redirect("/?saved=1");
}

// ---- 設定ページの操作中の、LED へのリアルタイムの反映 (保存はしない)
// 保存するのは「保存」を押したときだけで、保存しないまま再起動すると、元の値に戻る。
// 表示モードの切り替えだけは、押したときに保存する (BOOT ボタンと同じ)。
//
// 値は、WebSocket (/ws) で受け取る。スライドバーを動かしている間は、1 秒に 30 回ほど届くため、
// HTTP の POST (/live) のように、応答を待つ往復の時間がかからない。
// WebSocket が使えないときは、HTTP の POST (/live) で受け取る (ページのスクリプトが切り替える)

static AsyncWebSocket s_ws("/ws");

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

    req->redirect("/?saved=1");
}

// 登録のないアドレスへのアクセス (キャプティブポータル。説明は portal.h)
static void handleNotFound(AsyncWebServerRequest *req)
{
    Serial.printf("# http (portal) %s%s\n", req->host().c_str(), req->url().c_str());

    if (PORTAL_DIRECT_HTML) {
        req->send(200, "text/html; charset=utf-8", buildSettingsPage(false));
    } else {
        req->redirect(s_portalUrl);
    }
}

// ---------------------------------------------------------------- public

void portalBegin()
{
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

    // 無操作のタイムアウト
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

void portalSetSensor(float hPa, float tempC)
{
    s_hPa   = hPa;
    s_tempC = tempC;
}
