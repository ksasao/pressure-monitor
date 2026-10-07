/*
 * esp32c3-captive-portal-test - キャプティブポータルの検証用スケッチ
 *
 * iPhone などが、基板の Wi-Fi に接続したまま設定ページを開けるかを確認するための
 * スケッチです。本体のファームウェア (esp32c3-bmp581) とは独立しています。
 *
 * 次の記事の方式を再現しています。
 *   https://qiita.com/nak435/items/70223b72ae645c939477
 *     - アクセスポイントはパスワードなし (オープン)
 *     - DNS で全ての名前を基板のアドレスに向ける
 *     - 非同期 Web サーバ (ESPAsyncWebServer + AsyncTCP) で、
 *       どのアドレスへのアクセスにも、同じ HTML ページを 200 で返す
 *
 * 応答の方式を 3 つ用意しています。BOOT ボタンを押すたびに切り替わります
 * (LED1〜3 のうち、点灯しているものが現在の方式)。
 *   LED1 (方式 0) : 全てのアクセスに HTML を 200 で返す       (記事と同じ)
 *   LED2 (方式 1) : 全てのアクセスを設定ページへ転送する (302)
 *   LED3 (方式 2) : iOS の接続確認だけ "Success" と応答し、他は HTML
 *
 * LED4 : ページのスライダで変えられる色 (リアルタイムに反映されるかの確認用)
 * LED5 : 暗い水色 = 接続待ち / 緑 = スマホが接続している
 *
 * 必要ライブラリ (Arduino IDE: ライブラリマネージャ)
 *   - "ESP Async WebServer"  (ESP32Async 製)
 *   - "Async TCP"            (ESP32Async 製)
 *   - "FastLED"              3.7.0 以降
 *   (記事の動作確認は ESPAsyncWebServer 3.6.0 / AsyncTCP 3.3.2 / Arduino ESP32 Core 3.3.8)
 *
 * ボード設定 (Arduino IDE)
 *   Board            : ESP32C3 Dev Module
 *   USB CDC On Boot  : Enabled
 *
 * シリアル (115200bps) に、スマホの接続・切断と、HTTP のアクセスが出力されます。
 * iPhone が別の Wi-Fi へ移ったときに、何が起きたかを確認できます。
 */

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>

#define FASTLED_INTERNAL
#include <FastLED.h>

// ---------------------------------------------------------------- settings
// SSID とパスワード。パスワードが空 (または 8 文字未満) のときは、
// パスワードなし (オープン) で起動する (記事と同じ)
//   パスワードなし : WIFI:T:nopass;S:pressure-monitor-test;;
//   パスワードあり : WIFI:T:WPA;S:pressure-monitor-test;P:<パスワード>;;
static const char *AP_SSID     = "pressure-monitor-test";
static const char *AP_PASSWORD = "";

// ---------------------------------------------------------------- pin map
#define PIN_NEO 10                      // U2 IO10 -> R8 -> LED1 DIN
static const uint8_t PIN_BOOT = 9;      // U2 IO9 (タクトスイッチ, active low)
static const uint8_t NUM_LEDS = 5;

// ---------------------------------------------------------------- portal mode
enum PortalMode : uint8_t {
    PORTAL_HTML     = 0,    // 全てのアクセスに HTML を 200 で返す (記事と同じ)
    PORTAL_REDIRECT = 1,    // 全てのアクセスを設定ページへ転送する (302)
    PORTAL_IOS_OK   = 2,    // iOS の接続確認だけ "Success"、他は HTML
    PORTAL_MODE_COUNT
};

static const char *MODE_NAMES[PORTAL_MODE_COUNT] = {
    "0: HTML を 200 で直接返す (記事と同じ)",
    "1: 設定ページへ転送 (302)",
    "2: iOS の接続確認に Success を返す",
};

// ---------------------------------------------------------------- globals
AsyncWebServer server(80);
DNSServer      dnsServer;
CRGB           leds[NUM_LEDS];

static volatile uint8_t g_portalMode = PORTAL_HTML;
static CRGB    g_pageColor = CRGB::Black;       // ページのスライダで決まる色
static String  g_apIp;                          // 例: 192.168.4.1
static String  g_apUrl;                         // 例: http://192.168.4.1/

// ---------------------------------------------------------------- page
static const char PAGE_HEAD[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="ja"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>captive portal test</title>
<style>
body{font-family:sans-serif;margin:16px;max-width:520px}
h1{font-size:1.25em}
h2{font-size:1.05em;margin-top:24px}
.box{background:#eef;padding:8px 12px;border-radius:6px;line-height:1.7}
label{display:block;margin-top:10px}
input[type=range]{width:100%}
</style></head><body>
<h1>キャプティブポータルのテスト</h1>
<p>このページが見えていれば、基板に接続できています。</p>
<div class="box">応答の方式: <b>)HTML";

static const char PAGE_TAIL[] PROGMEM = R"HTML(</b><br>
通信: <span id="net">確認中...</span><br>
基板の経過時間: <span id="up">-</span></div>
<h2>LED4 の色 (リアルタイムに変わります)</h2>
<label>R <input type="range" id="r" min="0" max="255" value="0"></label>
<label>G <input type="range" id="g" min="0" max="255" value="0"></label>
<label>B <input type="range" id="b" min="0" max="255" value="0"></label>
<script>
var busy=false;
function send(){
  if(busy)return; busy=true;
  var q='r='+document.getElementById('r').value
       +'&g='+document.getElementById('g').value
       +'&b='+document.getElementById('b').value;
  fetch('/set_color?'+q).then(function(){busy=false;}).catch(function(){busy=false;});
}
['r','g','b'].forEach(function(id){
  document.getElementById(id).addEventListener('input',send);
});
function ping(){
  fetch('/ping').then(function(x){return x.text();}).then(function(t){
    document.getElementById('net').textContent='OK';
    document.getElementById('up').textContent=(parseInt(t,10)/1000).toFixed(1)+' 秒';
  }).catch(function(){document.getElementById('net').textContent='切断?';});
}
setInterval(ping,1000); ping();
</script></body></html>)HTML";

static String buildPage()
{
    String s;
    s.reserve(2200);
    s += FPSTR(PAGE_HEAD);
    s += MODE_NAMES[g_portalMode];
    s += FPSTR(PAGE_TAIL);
    return s;
}

// ---------------------------------------------------------------- handlers
static bool isAppleProbe(const String &url)
{
    return url == "/hotspot-detect.html" || url == "/library/test/success.html";
}

static void logRequest(AsyncWebServerRequest *req)
{
    String ua = req->header("User-Agent");
    if (ua.length() > 40) ua = ua.substring(0, 40);
    Serial.printf("[http] mode=%u  %s%s  UA=%s\n", (unsigned)g_portalMode,
                  req->host().c_str(), req->url().c_str(), ua.c_str());
}

static void sendPage(AsyncWebServerRequest *req)
{
    req->send(200, "text/html", buildPage());
}

// 「/」・登録のないアドレス・接続確認 (generate_204, hotspot-detect.html など) の全て
static void handlePortal(AsyncWebServerRequest *req)
{
    logRequest(req);

    switch (g_portalMode) {
        case PORTAL_HTML:
            // 記事と同じ。どのアドレスにも、同じ HTML を 200 で返す
            sendPage(req);
            break;

        case PORTAL_REDIRECT:
            // 基板のアドレスの「/」だけ HTML。それ以外は設定ページへ転送する
            if (req->url() == "/" && req->host() == g_apIp) sendPage(req);
            else req->redirect(g_apUrl);
            break;

        case PORTAL_IOS_OK:
            // iOS の接続確認だけ「繋がっている」と応答する
            if (isAppleProbe(req->url())) {
                req->send(200, "text/html",
                          "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
            } else {
                sendPage(req);
            }
            break;

        default:
            sendPage(req);
            break;
    }
}

// ---------------------------------------------------------------- LED / button
static void updateLeds()
{
    fill_solid(leds, NUM_LEDS, CRGB::Black);

    // LED1〜3: 現在の応答方式
    leds[g_portalMode] = CRGB(40, 30, 0);

    // LED4: ページのスライダの色
    leds[3] = g_pageColor;

    // LED5: Wi-Fi の状態
    leds[4] = (WiFi.softAPgetStationNum() > 0) ? CRGB(0, 60, 0) : CRGB(0, 40, 40);

    FastLED.show();
}

// BOOT ボタンを離したときに、応答の方式を切り替える
static void pollButton()
{
    static bool     prevDown = false;
    static uint32_t downAt   = 0;

    bool     down = (digitalRead(PIN_BOOT) == LOW);
    uint32_t now  = millis();

    if (down && !prevDown) downAt = now;
    if (!down && prevDown && (now - downAt) > 40) {
        g_portalMode = (g_portalMode + 1) % PORTAL_MODE_COUNT;
        Serial.printf("# portal mode -> %s\n", MODE_NAMES[g_portalMode]);
        Serial.println(F("#   (iPhone では、Wi-Fi 設定でこのネットワークを「削除」してから、"
                         "接続し直してください)"));
    }
    prevDown = down;
}

// ---------------------------------------------------------------- Wi-Fi events
static void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info)
{
    switch (event) {
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
            const uint8_t *m = info.wifi_ap_staconnected.mac;
            Serial.printf("[wifi] station CONNECTED     %02X:%02X:%02X:%02X:%02X:%02X\n",
                          m[0], m[1], m[2], m[3], m[4], m[5]);
            break;
        }
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
            const uint8_t *m = info.wifi_ap_stadisconnected.mac;
            Serial.printf("[wifi] station DISCONNECTED  %02X:%02X:%02X:%02X:%02X:%02X\n",
                          m[0], m[1], m[2], m[3], m[4], m[5]);
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------- setup / loop
void setup()
{
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);       // USB CDC On Boot = Enabled のときに使える
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) delay(10);

    Serial.println();
    Serial.println(F("=== esp32c3-captive-portal-test ==="));

    pinMode(PIN_BOOT, INPUT_PULLUP);

    FastLED.addLeds<WS2812B, PIN_NEO, GRB>(leds, NUM_LEDS);
    FastLED.setMaxPowerInVoltsAndMilliamps(3, 250);
    FastLED.setBrightness(255);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();

    WiFi.onEvent(onWiFiEvent);
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);

    bool useWpa = (strlen(AP_PASSWORD) >= 8);
    bool ok = useWpa ? WiFi.softAP(AP_SSID, AP_PASSWORD) : WiFi.softAP(AP_SSID);

    IPAddress ip = WiFi.softAPIP();
    g_apIp  = ip.toString();
    g_apUrl = String("http://") + g_apIp + "/";

    // 全ての名前を、基板のアドレスに向ける
    dnsServer.start(53, "*", ip);

    server.on("/set_color", HTTP_GET, [](AsyncWebServerRequest *req) {
        auto get = [&](const char *name) -> uint8_t {
            if (!req->hasParam(name)) return 0;
            long v = req->getParam(name)->value().toInt();
            return (uint8_t)((v < 0) ? 0 : (v > 255 ? 255 : v));
        };
        g_pageColor = CRGB(get("r"), get("g"), get("b"));
        req->send(200, "text/plain", "OK");
    });
    server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/plain", String(millis()));
    });
    server.on("/", HTTP_GET, handlePortal);
    server.onNotFound(handlePortal);
    server.begin();

    Serial.printf("# Wi-Fi AP %s  SSID=\"%s\"  %s\n", ok ? "started" : "FAILED", AP_SSID,
                  useWpa ? "(WPA2)" : "(open, no password)");
    Serial.printf("# URL: %s\n", g_apUrl.c_str());
    Serial.printf("# QR (Wi-Fi): %s\n",
                  useWpa ? "WIFI:T:WPA;S:<SSID>;P:<password>;;" : "WIFI:T:nopass;S:pressure-monitor-test;;");
    Serial.printf("# portal mode: %s\n", MODE_NAMES[g_portalMode]);
}

void loop()
{
    dnsServer.processNextRequest();
    pollButton();

    static uint32_t lastLed = 0;
    uint32_t now = millis();
    if (now - lastLed >= 100) {
        lastLed = now;
        updateLeds();
    }
    delay(1);
}
