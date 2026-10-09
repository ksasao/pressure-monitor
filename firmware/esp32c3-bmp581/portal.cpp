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

static uint32_t s_lastActivityMs = 0;      // 設定ページの最後の操作
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

// 設定ページの操作中に、値を LED へ、すぐに反映する (保存はしない)。
// 保存するのは「保存」を押したときだけで、保存しないまま再起動すると、元の値に戻る。
// 表示モードの切り替えだけは、押したときに保存する (BOOT ボタンと同じ)
static void handleLive(AsyncWebServerRequest *req)
{
    s_lastActivityMs = millis();

    const int32_t oldMode = g_set.mode;
    postInt(req, "bright", g_set.brightness);
    postInt(req, "limit",  g_set.deltaLimit);
    postInt(req, "range",  g_set.deltaRange);
    postInt(req, "hpf",    g_set.hpfShift);
    postInt(req, "trail",  g_set.trailStepTicks);
    postInt(req, "hue",    g_set.hue);
    postInt(req, "sat",    g_set.sat);
    postInt(req, "val",    g_set.val);
    if (req->hasParam("mode", true)) {
        // ページの番号は 1〜4、内部は 0〜3
        g_set.mode = (int32_t)req->getParam("mode", true)->value().toInt() - 1;
    }
    settingsClamp();

    if (g_set.mode != oldMode) {
        settingsSaveMode();
        Serial.printf("# mode -> %ld (%s) [settings page]\n", (long)g_set.mode + 1,
                      modeName((uint8_t)g_set.mode));
    }
    req->send(204);
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
    if (nowMs - s_lastActivityMs >= SETTINGS_IDLE_TIMEOUT_MS) {
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
