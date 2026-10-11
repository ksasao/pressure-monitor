#include "link.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "settings.h"
#include "battery.h"
#include "power.h"
#include "hub.h"

const uint8_t LINK_BROADCAST_ADDR[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static const uint8_t MAX_CHANNEL = 13;

static uint8_t    s_role    = LINK_ROLE_STANDALONE;
static bool       s_ready   = false;      // 送信 (またはハブへの受け渡し) ができる
static LinkPacket s_pkt;
static uint8_t    s_count   = 0;
static uint16_t   s_seq     = 0;
static uint32_t   s_sendErrors = 0;

// チャンネルの探索 (送信の装置)。ビーコンは Wi-Fi のタスクで受け取るので volatile
static volatile uint32_t s_beaconMs      = 0;      // 最後にビーコンを受け取った時刻
static volatile uint8_t  s_beaconChannel = 0;      // そのビーコンに書かれたチャンネル
static volatile bool     s_beaconFresh   = false;  // まだ loop() が見ていないビーコンがある
static bool     s_locked      = false;             // ハブのチャンネルに合わせている
static uint8_t  s_channel     = LINK_CHANNEL;      // 今のチャンネル
static uint32_t s_dwellStartMs = 0;                // 今のチャンネルで待ち始めた時刻

const char *linkRoleName(uint8_t role)
{
    switch (role) {
        case LINK_ROLE_STANDALONE: return "standalone";
        case LINK_ROLE_SENDER:     return "sender";
        case LINK_ROLE_HUB:        return "hub";
        default:                   return "?";
    }
}

bool linkAddBroadcastPeer()
{
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, LINK_BROADCAST_ADDR, 6);
    peer.channel = 0;                   // 今のチャンネル
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

// 送信の装置の受信 (Wi-Fi のタスク)。ハブのビーコンだけを見る
static void onSenderRecv(const esp_now_recv_info_t *, const uint8_t *data, int len)
{
    if (len < (int)sizeof(LinkBeacon)) return;
    if (data[0] != 'P' || data[1] != 'B' || data[2] != LINK_VERSION) return;
    const uint8_t ch = data[3];
    if (ch < 1 || ch > MAX_CHANNEL) return;
    s_beaconChannel = ch;
    s_beaconMs      = millis();
    s_beaconFresh   = true;
}

static void setChannel(uint8_t ch)
{
    s_channel = ch;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

// 送信の装置: Wi-Fi をステーションで起動し (どこにも接続しない)、ESP-NOW の
// ブロードキャストの宛先を登録する。チャンネルは、ハブのビーコンを探して合わせる
static bool beginSender()
{
    WiFi.persistent(false);             // Wi-Fi の設定をフラッシュに書かない
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.setTxPower(LINK_TX_POWER);

    if (esp_now_init() != ESP_OK) {
        Serial.println(F("# link: esp_now_init FAILED"));
        return false;
    }
    if (!linkAddBroadcastPeer()) {
        Serial.println(F("# link: esp_now_add_peer FAILED"));
        return false;
    }
    esp_now_register_recv_cb(onSenderRecv);

    setChannel(LINK_CHANNEL);           // まずは、ハブが自分でアクセスポイントになるときのチャンネル
    s_dwellStartMs = millis();
    Serial.printf("# link: sender, node %ld, MAC %s, searching hub from channel %u\n",
                  (long)g_set.node, WiFi.macAddress().c_str(), LINK_CHANNEL);
    return true;
}

void linkBegin()
{
    s_role = (uint8_t)g_set.role;
    if (s_role == LINK_ROLE_SENDER) {
        s_ready = beginSender();
    } else if (s_role == LINK_ROLE_HUB) {
        s_ready = hubBegin();
    } else {
        return;                         // 単体: 無線はオフのまま
    }
    // 無線を使っている間は、ライトスリープに入らない
    powerKeepAwake();
}

void linkService(uint32_t nowMs)
{
    if (s_role != LINK_ROLE_SENDER || !s_ready) return;

    if (s_beaconFresh) {
        s_beaconFresh = false;
        const uint8_t ch = s_beaconChannel;
        if (!s_locked || ch != s_channel) {
            // 隣のチャンネルで聞こえることがあるので、ビーコンに書かれたチャンネルに合わせる
            if (ch != s_channel) setChannel(ch);
            s_locked = true;
            Serial.printf("# link: hub found, channel %u\n", ch);
        }
    }

    if (s_locked) {
        if ((int32_t)(nowMs - s_beaconMs) >= (int32_t)LINK_BEACON_LOST_MS) {
            s_locked = false;
            s_count  = 0;               // 途中まで溜めたサンプルは捨てる
            s_dwellStartMs = nowMs;
            Serial.printf("# link: hub lost on channel %u, searching\n", s_channel);
        }
        return;
    }

    // 探索中: 一定時間ごとに、次のチャンネルへ (1〜13 を巡回)
    if (nowMs - s_dwellStartMs >= LINK_SCAN_DWELL_MS) {
        s_dwellStartMs = nowMs;
        setChannel((uint8_t)(s_channel % MAX_CHANNEL + 1));
    }
}

static void sendPacket()
{
    s_pkt.magic[0]  = 'P';
    s_pkt.magic[1]  = 'M';
    s_pkt.version   = LINK_VERSION;
    s_pkt.node      = (uint8_t)g_set.node;
    s_pkt.mode      = (uint8_t)g_set.mode;
    s_pkt.count     = s_count;
    s_pkt.seq       = s_seq++;
    s_pkt.reserved  = 0;
    uint32_t mv = batteryLastMilliVolts();
    s_pkt.batteryMv = (uint16_t)(mv > 0xFFFF ? 0xFFFF : mv);

    const size_t len = LINK_HEADER_BYTES + sizeof(LinkSample) * s_count;
    if (s_role == LINK_ROLE_HUB) {
        hubAcceptLocal((const uint8_t *)&s_pkt, len);
        return;
    }
    if (esp_now_send(LINK_BROADCAST_ADDR, (const uint8_t *)&s_pkt, len) != ESP_OK) {
        // 送信のキューがいっぱいなど。まれなら問題ない。数を数えて、ときどき知らせる
        if ((++s_sendErrors % 100) == 1) {
            Serial.printf("# link: send error (total %lu)\n", (unsigned long)s_sendErrors);
        }
    }
}

void linkPush(uint32_t nowMs, const SensorSample &s, int32_t hpf, const CRGB &color)
{
    if (!s_ready) return;
    // 送信の装置は、ハブのチャンネルが分かるまで送らない (連番は進めないので、欠落にならない)
    if (s_role == LINK_ROLE_SENDER && !s_locked) return;

    LinkSample &d = s_pkt.s[s_count];
    d.ms       = nowMs;
    d.raw      = s.raw;
    d.hpf      = hpf;
    d.r        = color.r;
    d.g        = color.g;
    d.b        = color.b;
    d.reserved = 0;
    s_pkt.tempCx100 = (int16_t)lroundf(s.tempC * 100.0f);

    if (++s_count >= LINK_SAMPLES_PER_PACKET) {
        sendPacket();
        s_count = 0;
    }
}
