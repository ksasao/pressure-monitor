#include "hub.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "config.h"
#include "link.h"
#include "hub_page.h"
#include "settings.h"

static AsyncWebServer s_server(80);
static AsyncWebSocket s_ws("/ws");

// 受信したパケット。ESP-NOW の受信は Wi-Fi のタスクで呼ばれるので、キューで loop() へ渡す
struct RxItem {
    uint32_t rxMs;
    int8_t   rssi;
    uint8_t  len;
    uint8_t  mac[6];
    uint8_t  data[sizeof(LinkPacket)];
};
static const size_t RX_RECORD_HEADER = 12;      // rxMs, rssi, len, mac (フレームの中の形)

static QueueHandle_t     s_queue   = nullptr;
static volatile uint32_t s_dropped = 0;         // キューがいっぱいで捨てた数
static uint8_t           s_ownMac[6];

// ネットワークの状態
enum HubNet {
    NET_CONNECTING,     // Wi-Fi (ルーター) に接続を試みている
    NET_STATION,        // ルーターに接続している
    NET_OWN_AP,         // 自分でアクセスポイントになっている
};
static HubNet   s_net          = NET_OWN_AP;
static uint32_t s_netStartMs   = 0;
static bool     s_mdnsStarted  = false;
static bool     s_wasConnected = false;
static uint32_t s_lastBeaconMs = 0;

// スマホへ送るフレーム
static const size_t FRAME_HEADER = 12;
static const size_t FRAME_MAX    = 2048;
static uint8_t  s_frame[FRAME_MAX];
static size_t   s_frameLen   = FRAME_HEADER;
static uint16_t s_frameCount = 0;
static uint32_t s_lastPushMs = 0;
static uint32_t s_wsSkipped  = 0;               // スマホへの送信が詰まって、送らなかったフレーム

// 統計 (シリアルへ、10 秒ごと)
static uint32_t s_rxCount    = 0;
static uint32_t s_lastStatMs = 0;

static bool validPacket(const uint8_t *data, int len)
{
    return len >= (int)LINK_HEADER_BYTES && len <= (int)sizeof(LinkPacket) &&
           data[0] == 'P' && data[1] == 'M' && data[2] == LINK_VERSION &&
           data[3] == (uint8_t)g_set.group;             // 別のグループの送信の装置は無視する
}

static void enqueue(const uint8_t *mac, int8_t rssi, const uint8_t *data, size_t len)
{
    RxItem it;
    it.rxMs = millis();
    it.rssi = rssi;
    it.len  = (uint8_t)len;
    memcpy(it.mac, mac, 6);
    memcpy(it.data, data, len);
    if (xQueueSend(s_queue, &it, 0) != pdTRUE) s_dropped++;
}

// ESP-NOW の受信 (Wi-Fi のタスク)。重い処理はしない
static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!s_queue || !validPacket(data, len)) return;
    const int8_t rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    enqueue(info->src_addr, rssi, data, (size_t)len);
}

void hubAcceptLocal(const uint8_t *pkt, size_t len)
{
    if (!s_queue || !validPacket(pkt, (int)len)) return;
    enqueue(s_ownMac, 0, pkt, len);
}

static void handleRoot(AsyncWebServerRequest *req)
{
    req->send(200, "text/html; charset=utf-8", (const uint8_t *)HUB_PAGE_HTML, strlen(HUB_PAGE_HTML));
}

static void handleNotFound(AsyncWebServerRequest *req)
{
    req->send(404, "text/plain", "not found");
}

static void appendJsonEscaped(String &j, const char *s)
{
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') j += '\\';
        if ((uint8_t)*s >= 0x20) j += *s;
    }
}

static uint8_t currentChannel();

// ネットワークの状況 (表示ページの上部に出す。ルーターから割り当てられた IP アドレスなど)
static void handleInfo(AsyncWebServerRequest *req)
{
    const bool staUp = (s_net != NET_OWN_AP) && (WiFi.status() == WL_CONNECTED);
    const bool apUp  = (WiFi.getMode() & WIFI_MODE_AP) != 0;
    String j;
    j.reserve(200);
    j += F("{\"net\":\"");
    j += (s_net == NET_OWN_AP) ? F("own_ap") : (staUp ? F("station") : F("connecting"));
    j += F("\",\"ssid\":\"");
    appendJsonEscaped(j, g_set.wifiSsid);
    j += F("\",\"sta_ip\":\"");
    if (staUp) j += WiFi.localIP().toString();
    j += F("\",\"mdns\":\"");
    if (staUp && s_mdnsStarted) { j += HUB_MDNS_NAME; j += F(".local"); }
    j += F("\",\"ap_ssid\":\"");
    if (apUp) j += HUB_SSID;
    j += F("\",\"ap_ip\":\"");
    if (apUp) j += WiFi.softAPIP().toString();
    j += F("\",\"group\":");
    j += String(g_set.group);
    j += F(",\"channel\":");
    j += String(currentChannel());
    j += F("}");
    req->send(200, "application/json", j);
}

// ルーターから割り当てられた IP アドレス (未接続なら "-")
static String staIpText()
{
    return (s_net != NET_OWN_AP && WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString()
                                                                  : String("-");
}

// 自分でアクセスポイントになる (Wi-Fi が未設定のとき、またはルーターにつながらないとき)
static bool startOwnAp()
{
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
    // ESP-NOW の送信 (ビーコン) は、ステーションのインターフェースから出すので、両方を起動する
    WiFi.mode(WIFI_AP_STA);
    bool ok = WiFi.softAP(HUB_SSID, nullptr, (int)g_set.hubChannel, 0, HUB_MAX_CLIENTS);
    WiFi.setTxPower(AP_TX_POWER);
    esp_wifi_get_mac(WIFI_IF_AP, s_ownMac);
    s_net = NET_OWN_AP;

    Serial.printf("# hub: Wi-Fi AP %s, channel %ld, group %ld\n", ok ? "started" : "FAILED",
                  (long)g_set.hubChannel, (long)g_set.group);
    Serial.printf("#   SSID : %s (open, no password)\n", HUB_SSID);
    Serial.printf("#   URL  : http://%s/\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("#   QR (Wi-Fi): WIFI:T:nopass;S:%s;;\n", HUB_SSID);
    return ok;
}

// ルーターに接続する。結果は hubService() で見る。
// HUB_KEEP_AP_WITH_STA なら、自分のアクセスポイントも立ち上げたままにする。スマホをそこに
// つなげば、PC がなくても、ルーターから割り当てられた IP アドレスを確認できる (表示ページの上部)。
// アクセスポイントのチャンネルは、ルーターのチャンネルに合わせて変わる
static void startStation()
{
    if (HUB_KEEP_AP_WITH_STA) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(HUB_SSID, nullptr, (int)g_set.hubChannel, 0, HUB_MAX_CLIENTS);
    } else {
        WiFi.mode(WIFI_STA);
    }
    // 省電力 (モデムスリープ) を切る。眠っている間は、ESP-NOW を受信できない
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(g_set.wifiSsid, g_set.wifiPass[0] ? g_set.wifiPass : nullptr);
    WiFi.setTxPower(AP_TX_POWER);
    esp_wifi_get_mac(WIFI_IF_STA, s_ownMac);
    s_net        = NET_CONNECTING;
    s_netStartMs = millis();
    Serial.printf("# hub: connecting to Wi-Fi \"%s\"\n", g_set.wifiSsid);
    if (HUB_KEEP_AP_WITH_STA) {
        Serial.printf("#   AP   : %s (open) http://%s/  <- IP address is shown on this page\n",
                      HUB_SSID, WiFi.softAPIP().toString().c_str());
    }
}

bool hubBegin()
{
    s_queue = xQueueCreate(64, sizeof(RxItem));

    WiFi.persistent(false);             // Wi-Fi の設定をフラッシュに書かない
    bool ok = true;
    if (g_set.wifiSsid[0]) {
        startStation();
    } else {
        ok = startOwnAp();
    }

    if (esp_now_init() != ESP_OK) {
        Serial.println(F("# hub: esp_now_init FAILED"));
        ok = false;
    } else {
        esp_now_register_recv_cb(onRecv);
        if (!linkAddBroadcastPeer()) Serial.println(F("# hub: esp_now_add_peer FAILED"));
    }

    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/info", HTTP_GET, handleInfo);
    s_server.addHandler(&s_ws);
    s_server.onNotFound(handleNotFound);
    s_server.begin();
    return ok;
}

static uint8_t currentChannel()
{
    uint8_t primary = 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);
    return primary;
}

// ルーターへの接続の状態を見る。つながったら IP アドレスを知らせ、
// 時間内につながらなければ、自分でアクセスポイントになる
static void serviceNetwork(uint32_t nowMs)
{
    if (s_net == NET_OWN_AP) return;

    const bool connected = (WiFi.status() == WL_CONNECTED);
    if (connected && !s_wasConnected) {
        s_net = NET_STATION;
        const String ip = WiFi.localIP().toString();
        Serial.printf("# hub: connected to \"%s\", channel %u, RSSI %d dBm, MAC %s\n",
                      g_set.wifiSsid, currentChannel(), (int)WiFi.RSSI(), WiFi.macAddress().c_str());
        Serial.printf("#   URL  : http://%s/  (http://%s.local/)\n", ip.c_str(), HUB_MDNS_NAME);
        settingsSaveHubLastIp(ip);
        if (!s_mdnsStarted && MDNS.begin(HUB_MDNS_NAME)) {
            MDNS.addService("http", "tcp", 80);
            s_mdnsStarted = true;
        }
    } else if (!connected && s_wasConnected) {
        // 一度つながった後に切れたときは、自動で再接続する (アクセスポイントには切り替えない)
        Serial.println(F("# hub: Wi-Fi disconnected, reconnecting"));
    }
    s_wasConnected = connected;

    if (s_net == NET_CONNECTING && !connected && nowMs - s_netStartMs >= HUB_STA_TIMEOUT_MS) {
        Serial.printf("# hub: could not connect to \"%s\" (status %d), starting own AP\n",
                      g_set.wifiSsid, (int)WiFi.status());
        startOwnAp();
    }
}

// ビーコン (送信の装置が、ハブのチャンネルを見つけるための合図)。
// ルーターにつながっていない (チャンネルが定まらない) 間は送らない
static void serviceBeacon(uint32_t nowMs)
{
    if (s_net == NET_CONNECTING) return;
    if (s_net == NET_STATION && WiFi.status() != WL_CONNECTED) return;
    if (nowMs - s_lastBeaconMs < LINK_BEACON_INTERVAL_MS) return;
    s_lastBeaconMs = nowMs;

    LinkBeacon b;
    b.magic[0] = 'P';
    b.magic[1] = 'B';
    b.version  = LINK_VERSION;
    b.channel  = currentChannel();
    b.hubMs    = nowMs;
    b.group    = (uint8_t)g_set.group;
    memset(b.reserved, 0, sizeof(b.reserved));
    esp_now_send(LINK_BROADCAST_ADDR, (const uint8_t *)&b, sizeof(b));
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// たまったフレームを、全てのスマホへ送る。誰も見ていなくても、溜めずに捨てる
static void pushFrame(uint32_t nowMs)
{
    s_frame[0] = 'H';
    s_frame[1] = 1;
    s_frame[2] = (uint8_t)s_frameCount;
    s_frame[3] = (uint8_t)(s_frameCount >> 8);
    put32(s_frame + 4, nowMs);
    put32(s_frame + 8, s_dropped);

    if (s_ws.count() > 0) {
        // 送信が詰まっているスマホがあるときは、このフレームを送らない (メモリを使い切らないため)
        if (s_ws.availableForWriteAll()) {
            s_ws.binaryAll(s_frame, s_frameLen);
        } else {
            s_wsSkipped++;
        }
    }
    s_frameLen   = FRAME_HEADER;
    s_frameCount = 0;
    s_lastPushMs = nowMs;
}

void hubService(uint32_t nowMs)
{
    if (!s_queue) return;

    serviceNetwork(nowMs);
    serviceBeacon(nowMs);

    RxItem it;
    while (xQueueReceive(s_queue, &it, 0) == pdTRUE) {
        const size_t need = RX_RECORD_HEADER + it.len;
        if (s_frameLen + need > FRAME_MAX) pushFrame(nowMs);   // 入りきらなければ、先に送る

        uint8_t *p = s_frame + s_frameLen;
        put32(p, it.rxMs);
        p[4] = (uint8_t)it.rssi;
        p[5] = it.len;
        memcpy(p + 6, it.mac, 6);
        memcpy(p + RX_RECORD_HEADER, it.data, it.len);
        s_frameLen += need;
        s_frameCount++;
        s_rxCount++;
    }

    // 受信がなくても、1 秒に 1 回は送る (スマホが、接続が生きていることを確かめられる)
    const uint32_t since = nowMs - s_lastPushMs;
    if ((s_frameCount > 0 && since >= HUB_PUSH_INTERVAL_MS) || since >= 1000) {
        pushFrame(nowMs);
    }

    if (nowMs - s_lastStatMs >= 10000) {
        s_lastStatMs = nowMs;
        s_ws.cleanupClients();          // 切れた WebSocket の接続を片づける
        // IP アドレスも毎回出す (接続したときの 1 回だけだと、シリアルモニタを後から開くと見逃す)
        Serial.printf("# hub: %s ip %s ch %u, rx %lu pkt / 10 s, dropped %lu, ws clients %u, ws skipped %lu\n",
                      s_net == NET_OWN_AP ? "own AP" : (s_net == NET_STATION ? "station" : "connecting"),
                      s_net == NET_OWN_AP ? WiFi.softAPIP().toString().c_str() : staIpText().c_str(),
                      currentChannel(), (unsigned long)s_rxCount, (unsigned long)s_dropped,
                      (unsigned)s_ws.count(), (unsigned long)s_wsSkipped);
        s_rxCount = 0;
    }
}
