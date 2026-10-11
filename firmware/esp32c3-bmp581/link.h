/*
 * link.h - 複数台の表示 (ESP-NOW の送信と、パケットの形式)
 *
 * 役割が「送信」の装置は、気圧のデータを、ESP-NOW のブロードキャストで送ります。
 * 宛先を登録する手順 (ペアリング) は要りません。同じチャンネル (LINK_CHANNEL) の
 * ハブが受け取ります (hub.h)。ブロードキャストは再送しないため、届かなかったパケットは、
 * 連番 (seq) の抜けで分かります。
 *
 * ハブも、自分の気圧を、同じ形式のパケットにして、受け取ったものと同じように扱います。
 *
 * チャンネルの自動探索:
 * ハブは、Wi-Fi (ルーター) に接続すると、ルーターのチャンネルで動きます。そのため、ハブは
 * LINK_BEACON_INTERVAL_MS ごとに、自分のチャンネルを入れた合図 (ビーコン) を送ります。
 * 送信の装置は、1〜13 のチャンネルを順に聞いて、ビーコンが聞こえたら、そのチャンネルに
 * 合わせます (隣のチャンネルで聞こえることがあるため、ビーコンに書かれたチャンネルを使う)。
 * ビーコンが LINK_BEACON_LOST_MS 途切れたら、探し直します。探している間は、送りません。
 *
 * ビーコンの形式 (8 バイト)
 *   0  magic[2]  'P','B'
 *   2  version   LINK_VERSION
 *   3  channel   ハブのチャンネル
 *   4  hubMs     ハブの millis
 *
 * パケットの形式 (リトルエンディアン、64 バイト。ハブの画面のスクリプトも、この形式で読む)
 *   0  magic[2]   'P','M'
 *   2  version    LINK_VERSION
 *   3  node       装置の番号 (1〜LINK_MAX_NODE)
 *   4  mode       表示モード (0〜3)
 *   5  count      サンプル数 (1〜LINK_SAMPLES_PER_PACKET)
 *   6  seq        連番 (uint16)
 *   8  batteryMv  最後に測った電池電圧 [mV]
 *  10  tempCx100  温度 [0.01 ℃] (int16)
 *  12  reserved   0 (uint32)
 *  16  サンプル x count (各 16 バイト)
 *        0  ms    そのサンプルを測った時刻 (送信側の millis)
 *        4  raw   気圧 (1/64 Pa)。hPa = raw / 6400
 *        8  hpf   HPF の出力 (1/64 Pa)
 *       12  r,g,b LED1 の色 (明るさの最大値を掛ける前)
 *       15  reserved
 */
#pragma once

#include <stdint.h>

#include "config.h"
#include "led_types.h"
#include "sensor.h"

enum LinkRole {
    LINK_ROLE_STANDALONE = 0,   // 単体 (無線はオフ)
    LINK_ROLE_SENDER     = 1,   // 送信
    LINK_ROLE_HUB        = 2,   // ハブ
    LINK_ROLE_COUNT
};

static const uint8_t LINK_VERSION  = 1;
static const int32_t LINK_MAX_NODE = 20;

struct __attribute__((packed)) LinkSample {
    uint32_t ms;
    int32_t  raw;
    int32_t  hpf;
    uint8_t  r, g, b;
    uint8_t  reserved;
};

struct __attribute__((packed)) LinkPacket {
    uint8_t    magic[2];
    uint8_t    version;
    uint8_t    node;
    uint8_t    mode;
    uint8_t    count;
    uint16_t   seq;
    uint16_t   batteryMv;
    int16_t    tempCx100;
    uint32_t   reserved;
    LinkSample s[LINK_SAMPLES_PER_PACKET];
};

static const size_t LINK_HEADER_BYTES = sizeof(LinkPacket) - sizeof(LinkSample) * LINK_SAMPLES_PER_PACKET;

struct __attribute__((packed)) LinkBeacon {
    uint8_t  magic[2];
    uint8_t  version;
    uint8_t  channel;
    uint32_t hubMs;
};

// ESP-NOW のブロードキャストの宛先を登録する (送信の装置とハブの両方で使う)。
// チャンネルは 0 (今のチャンネル) にするので、チャンネルを変えても登録し直さなくてよい
bool linkAddBroadcastPeer();
extern const uint8_t LINK_BROADCAST_ADDR[6];

const char *linkRoleName(uint8_t role);

// 役割 (g_set.role) に合わせて、無線を起動する。設定モードでは呼ばない。
// 単体なら何もしない
void linkBegin();

// 毎ティック、測った値を渡す。LINK_SAMPLES_PER_PACKET 個たまったら、送る
// (ハブなら、自分の受信のキューに入れる)。単体なら何もしない
void linkPush(uint32_t nowMs, const SensorSample &s, int32_t hpf, const CRGB &color);

// ループから毎回呼ぶ。送信の装置のチャンネルの探索 (ハブ・単体では何もしない)
void linkService(uint32_t nowMs);
