#include "devlog.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <time.h>
#include <array>
#include <memory>

#include <esp_mac.h>

#include "settings.h"
#include "display.h"
#include "link.h"
#include "modes.h"
#include "version.h"
#include "boot.h"
#include "high_pass_filter.h"

static const uint32_t SECTOR      = 4096;
static const uint32_t DATA_START  = SECTOR;          // 先頭の 1 セクタがヘッダ
static const uint32_t STOP_OFFSET = 64;              // 停止の情報の位置
static const uint8_t  TAG_FRAME   = 0xA5;              // 毎サンプルのフレーム (版 4 まで。読むだけ)
static const uint8_t  TAG_PACK    = 0xA6;              // 毎サンプルのフレーム (版 5。読むだけ)
static const uint8_t  TAG_FIFO    = 0xA9;              // 毎サンプルのフレーム (版 6 から。センサの間隔)
static const uint8_t  TAG_BOOT    = 0xB0;
static const uint8_t  TAG_BATT    = 0xBA;
static const uint8_t  TAG_AVG     = 0xA7;              // 平均したサンプル (記録の間隔が 1 秒以上)
static const uint8_t  TAG_AVGD    = 0xA8;              // 平均したサンプル (前との差。版 5 から)
static const uint32_t FRAME_HDR   = 13;
static const uint8_t  MAX_DELTAS  = 29;              // 1 フレーム = 30 サンプル (約 1 秒)
static const uint32_t FRAME_MAX   = FRAME_HDR + MAX_DELTAS * 3;
static const uint32_t PACK_HDR    = 15;
static const uint8_t  PACK_MAX    = 119;             // 1 フレーム = 120 サンプル (約 4 秒)
static const uint32_t PACK_DT     = 33;              // 1 バイトの符号の、前のサンプルからの時間 [ms]
static const uint8_t  PACK_TEMP_EVERY = 30;          // 温度を書き直す間隔 (サンプル数。約 1 秒)
static const uint32_t PACK_CODES_MAX  = PACK_MAX * 4 + (PACK_MAX / PACK_TEMP_EVERY + 1) * 2;
static const uint32_t PACK_FRAME_MAX  = PACK_HDR + PACK_CODES_MAX;
static const uint32_t FIFO_HDR    = 17;
static const uint8_t  FIFO_MAX    = 239;             // 1 フレーム = 240 サンプル (約 8 秒)
static const uint8_t  FIFO_TEMP_EVERY = 30;          // 温度を書き直す間隔 (サンプル数。約 1 秒)
static const uint32_t FIFO_CODES_MAX  = FIFO_MAX * 3 + (FIFO_MAX / FIFO_TEMP_EVERY + 1) * 2;
static const uint32_t FIFO_FRAME_MAX  = FIFO_HDR + FIFO_CODES_MAX;
static const uint32_t READ_FRAME_MAX  = FIFO_FRAME_MAX > PACK_FRAME_MAX ? FIFO_FRAME_MAX : PACK_FRAME_MAX;
// センサの測定の間隔 [µs]。センサの内蔵の発振器で決まり、30Hz より少し速い (実測 約 33305)。
// 記録しながら求める。求める前と、値がおかしいときは、これを使う
static const float    FIFO_PERIOD_NOMINAL_US = 1e6f / 30.0f;
// 読んだ時刻が、見込みより、これ以上遅れたら、サンプルが抜けたとみなす (新しいフレームにする)
static const int32_t  FIFO_GAP_US = 100000;
static const uint32_t BOOT_FRAME  = 6;
static const uint32_t BATT_FRAME  = 4;
static const uint32_t AVG_FRAME   = 14;
static const uint32_t AVGD_FRAME  = 8;
static const uint32_t RTC_MAGIC   = 0x504D4C47;      // "PMLG"
static const uint8_t  LOG_VERSION = 6;               // 2〜5 の記録も読める (devlog.h)
static const uint8_t  FLAG_WIFI_OFF = 1;

struct __attribute__((packed)) LogHeader {
    char     magic[4];
    uint8_t  version;           // 4 (3 = 記録の間隔がない、2 = 電池電圧と装置の情報もない)
    uint8_t  session;
    int16_t  tzMin;
    int64_t  startEpochMs;      // 記録を始めたときの時刻 (スマホの時計)
    int64_t  hpfPrevX;          // 記録を始めたときの HPF の状態
    int64_t  hpfY;
    uint8_t  hpfInit;
    uint8_t  mode;
    // ここから版 3
    uint8_t  role;              // 記録を始めたときの役割 (link.h の LinkRole)
    uint8_t  flags;             // FLAG_WIFI_OFF: Wi-Fi をオフにして記録した
    char     name[NODE_NAME_MAX + 1];   // 装置の名前
    // ここから版 4
    uint32_t intervalMs;        // 記録の間隔 [ms]。0 = 毎サンプル (30Hz)。1000 以上は、その間の平均
    uint8_t  reserved[7];
};
static_assert(sizeof(LogHeader) <= STOP_OFFSET, "header too large");

struct __attribute__((packed)) StopInfo {
    uint8_t  stopped;           // 0xFF = 記録中、0 = 停止した
    uint8_t  pad[3];
    uint32_t stopRelMs;         // 停止したときの rel
    int64_t  stopPhoneMs;       // 停止したときのスマホの時計。-1 = なし (補正しない)
};

static const esp_partition_t *s_part = nullptr;
static HighPassFilter *s_hpf = nullptr;
static LogHeader s_header;
static bool      s_valid = false;          // ヘッダがある (記録がある)
static StopInfo  s_stop;

static bool     s_recording = false;
static bool     s_full      = false;
static volatile uint32_t s_count = 0;      // フラッシュに書き終えたサンプル数
static volatile uint32_t s_end   = DATA_START;   // 書き終えた位置
static uint32_t s_erasedTo  = DATA_START;  // ここまで消去済み

// 作りかけのフレーム (TAG_FIFO)
static uint8_t  s_frame[FIFO_FRAME_MAX];
static bool     s_open   = false;
static uint8_t  s_n      = 0;              // 先頭のあとのサンプル数
static uint32_t s_len    = 0;              // 符号のバイト数
// サンプルを読んだ時刻 (フレームの先頭のサンプルを読んだ時刻 s_baseUs からの時間 [µs])。
// 時刻は、基板の水晶の時計 (esp_timer_get_time)。フレームを閉じるときに、センサが測った時刻を求める
static int32_t  s_readUs[FIFO_MAX + 1];
static int64_t  s_baseUs  = 0;             // フレームの先頭のサンプルを読んだ時刻 [µs]
static uint32_t s_baseG   = 0;             // フレームの先頭のサンプルの番号 (periodTrack の番号)
static int32_t  s_envUs   = 0;             // 読んだ時刻の下側 (s_readUs[k] - k x 間隔 の最小)
// センサの測定の間隔の見込み [µs]。記録していないときも、ずっと求め続ける (periodTrack)。
// 再起動 (設定モードとの切り替え) のあとも使えるように、RTC メモリに残す
static RTC_NOINIT_ATTR float    s_periodUs;
static RTC_NOINIT_ATTR uint32_t s_periodMagic;
static const uint32_t PERIOD_MAGIC    = 0x50455244;    // "PERD" (まだ 30Hz の値)
static const uint32_t PERIOD_MEASURED = 0x5045524D;    // "PERM" (実測した値)
// 実測したあとは、前の値から、これ以上離れた値は使わない (サンプルの抜けで、数え違えたとみなす)。
// 温度による変化は、これよりずっと小さく、ゆっくり
static const float    PERIOD_MAX_STEP = 500e-6f;
// 間隔を求めるための、読んだ時刻の下側の点 (256 サンプルごとに、遅れが最小のサンプル)。
// 8 個 (約 68 秒) 集めて、古い 4 個の中の最小と、新しい 4 個の中の最小を結ぶ傾きを、間隔とする
static const uint32_t TRACK_BLOCK = 256;
static const uint8_t  TRACK_BLOCKS = 8;
static int64_t  s_trkG[TRACK_BLOCKS], s_trkT[TRACK_BLOCKS];   // サンプルの番号と、読んだ時刻 [µs]
static int64_t  s_trkLastT = 0;            // 前のサンプルを読んだ時刻
static uint8_t  s_trkN = 0;                // たまった点の数
static uint32_t s_trkG0 = 0;               // 次のサンプルの番号 (抜けたら 0 から)
static int64_t  s_blkG = 0, s_blkT = 0;    // 作りかけのブロックの最小の点
static double   s_blkE = 0;
// 前のフレームの最後のサンプルの時刻 [ms]。次のフレームの先頭を、これより前にしない
static uint32_t s_prevEndMs = 0;
static bool     s_prevEndOk = false;
static bool     s_gap      = true;         // サンプルが抜けたかもしれない (次は新しいフレーム)
static int16_t  s_temp   = 0;              // フレームの中の今の温度 [0.01 ℃]
static uint8_t  s_tempAge = 0;             // 温度を書いてからのサンプル数
static uint32_t s_lastRel = 0;
static int32_t  s_lastRaw = 0;
static int32_t  s_shift   = 0;
static uint32_t s_battMv  = UINT32_MAX;    // 最後に書いた電池電圧 (UINT32_MAX = まだ書いていない)

// 記録の間隔が 1 秒以上のときの、平均の途中 (TAG_AVG のフレームにする)
static uint32_t s_accN     = 0;
static int64_t  s_accRaw   = 0;
static int64_t  s_accHpf   = 0;
static int32_t  s_accTemp  = 0;            // 0.01 ℃
static uint64_t s_accRel   = 0;
static uint32_t s_nextRel  = 0;            // この時刻 (rel) を過ぎたら書く。0 = 最初のサンプルで決める
// 前に書いた平均 (TAG_AVGD は、これとの差を書く)。再起動のあとは、最初を TAG_AVG で書く
static bool     s_avgHave  = false;
static uint32_t s_avgRel   = 0;
static int32_t  s_avgRaw   = 0;
static int16_t  s_avgTemp  = 0;

// 記録中であることを、再起動をまたいで覚えておく (電源が切れると消える)
static RTC_NOINIT_ATTR uint32_t s_rtcMagic;

// ページからの操作 (Web サーバのタスクで受け取り、loop() で行う)
enum Cmd : uint8_t { CMD_NONE, CMD_START, CMD_STOP, CMD_CLEAR };
static volatile Cmd     s_cmd = CMD_NONE;
static volatile int64_t s_cmdEpoch = 0;
static volatile int32_t s_cmdTz = 0;
static volatile bool    s_cmdWifiOff = false;
static volatile uint32_t s_cmdInterval = 0;
static uint32_t s_rebootAtMs = 0;          // 0 以外: この時刻に再起動して、Wi-Fi をオフにする

// ---------------------------------------------------------------- helpers

static int64_t nowEpochMs()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static uint32_t nowRel()
{
    const int64_t r = nowEpochMs() - s_header.startEpochMs;
    return r < 0 ? 0 : (uint32_t)r;
}

// 記録を始めてからの時間 [µs]
static int64_t nowRelUs()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const int64_t r = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec - s_header.startEpochMs * 1000;
    return r < 0 ? 0 : r;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put24(uint8_t *p, uint32_t v) { put16(p, v); p[2] = (uint8_t)(v >> 16); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t get24(const uint8_t *p) { return get16(p) | (p[2] << 16); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

// フラッシュを消去・書き込みする前に、LED への送信を終わらせる (送信中に止まると、色が化ける)
static void flashPrepare()
{
    displayWait();
}

static void eraseUpTo(uint32_t end)
{
    while (s_erasedTo < end && s_erasedTo < s_part->size) {
        flashPrepare();
        esp_partition_erase_range(s_part, s_erasedTo, SECTOR);
        s_erasedTo += SECTOR;
    }
}

// s_end と s_count は、CSV を作るタスクが読むので、組にして書き換える
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// 本体を書いてから、最後に tag を書く (tag のないフレームは、読むときに無視される)。
// samples は、このフレームのサンプル数
static void writeFrame(const uint8_t *data, uint32_t len, uint32_t samples)
{
    eraseUpTo(s_end + len);
    flashPrepare();
    esp_partition_write(s_part, s_end + 1, data + 1, len - 1);
    esp_partition_write(s_part, s_end, data, 1);
    portENTER_CRITICAL(&s_mux);
    s_end   = s_end + len;
    s_count = s_count + samples;
    portEXIT_CRITICAL(&s_mux);
}

static void writeStop(int64_t phoneMs)
{
    memset(&s_stop, 0xFF, sizeof(s_stop));
    s_stop.stopped     = 0;
    s_stop.stopRelMs   = s_recording ? max(nowRel(), s_lastRel) : s_lastRel;   // スマホの時計と同じ時点
    s_stop.stopPhoneMs = phoneMs > 0 ? phoneMs : -1;
    flashPrepare();
    esp_partition_write(s_part, STOP_OFFSET, &s_stop, sizeof(s_stop));
}

// TAG_FIFO の k 番目のサンプルの時刻 [ms] (先頭の時刻 rel0 と、間隔 periodUs [µs] から)
static uint32_t fifoRel(uint32_t rel0, uint32_t periodUs, uint32_t k)
{
    return rel0 + (uint32_t)(((uint64_t)k * periodUs + 500) / 1000);
}

// センサの測定の間隔を求める (センサが測った値ごとに呼ぶ。tUs は読んだ時刻 [µs]、基板の水晶の時計)。
// 読んだ時刻 = 測った時刻 + 遅れ (0〜約 33ms。基板が起きる間隔の分)。基板が起きる間隔 (33ms) と、
// センサの間隔 (約 33.3ms) が少し違うので、遅れは約 4 秒で 0〜33ms を一巡する (のこぎり波)。
// そこで、遅れがほぼ 0 のサンプル (読んだ時刻の下側の点) を、離れた 2 か所で探し、その傾きを間隔とする
static void periodTrack(int64_t tUs)
{
    if ((s_periodMagic != PERIOD_MAGIC && s_periodMagic != PERIOD_MEASURED) ||
        !(fabsf(s_periodUs / FIFO_PERIOD_NOMINAL_US - 1.0f) < 0.02f)) {
        s_periodUs    = FIFO_PERIOD_NOMINAL_US;
        s_periodMagic = PERIOD_MAGIC;
    }
    // 読んだ時刻が、見込みより大きく飛んだ: サンプルが抜けた (数え直す)
    if (s_trkG0 > 0 && tUs - s_trkLastT > (int64_t)s_periodUs + FIFO_GAP_US) {
        s_trkN  = 0;
        s_trkG0 = 0;
    }
    s_trkLastT = tUs;
    const uint32_t g = s_trkG0++;
    const double   e = (double)tUs - (double)g * s_periodUs;    // 小さいほど、遅れが小さい
    const uint32_t b = g % TRACK_BLOCK;
    if (b == 0 || e < s_blkE) { s_blkE = e; s_blkG = g; s_blkT = tUs; }
    if (b != TRACK_BLOCK - 1) return;

    // ブロックの最小の点を、たまった点に加える (古いものから捨てる)
    if (s_trkN == TRACK_BLOCKS) {
        memmove(s_trkG, s_trkG + 1, sizeof(s_trkG[0]) * (TRACK_BLOCKS - 1));
        memmove(s_trkT, s_trkT + 1, sizeof(s_trkT[0]) * (TRACK_BLOCKS - 1));
        s_trkN--;
    }
    s_trkG[s_trkN] = s_blkG;
    s_trkT[s_trkN] = s_blkT;
    s_trkN++;
    if (s_trkN < TRACK_BLOCKS) return;

    // 古い半分と新しい半分で、それぞれ下側の点を選び、傾きを求める
    const double T = s_periodUs;
    uint8_t i1 = 0, i2 = TRACK_BLOCKS / 2;
    double  v1 = 1e300, v2 = 1e300;
    for (uint8_t i = 0; i < TRACK_BLOCKS; i++) {
        const double v = (double)(s_trkT[i] - s_trkT[0]) - (double)(s_trkG[i] - s_trkG[0]) * T;
        if (i < TRACK_BLOCKS / 2) { if (v < v1) { v1 = v; i1 = i; } }
        else                      { if (v < v2) { v2 = v; i2 = i; } }
    }
    const float Tn = (float)((double)(s_trkT[i2] - s_trkT[i1]) / (double)(s_trkG[i2] - s_trkG[i1]));
    if (!(fabsf(Tn / FIFO_PERIOD_NOMINAL_US - 1.0f) < 0.02f)) return;   // ±2% を超えるのは、おかしい
    if (s_periodMagic == PERIOD_MEASURED && !(fabsf(Tn / s_periodUs - 1.0f) < PERIOD_MAX_STEP)) {
        // 前の値から大きく離れた: 見落としたサンプルの抜けがあったとみなし、集め直す
        s_trkN  = 0;
        s_trkG0 = 0;
        return;
    }
    s_periodUs    = Tn;
    s_periodMagic = PERIOD_MEASURED;
}

// サンプルが抜けた: サンプルの番号が数えられなくなるので、間隔を求め直す (今の間隔の見込みは残す)
static void periodTrackReset()
{
    s_trkN  = 0;
    s_trkG0 = 0;
}

// フレームの先頭のサンプルを測った時刻 (記録を始めてからの時間 [µs]) と、間隔。
// 測った時刻 = 読んだ時刻の下側の線 (c + 番号 x 間隔)。c は、このフレームのサンプルと、periodTrack が
// 集めた最近 (約 1 分) の下側の点の中で、(読んだ時刻 - 番号 x 間隔) が最小のもの。フレームが短くても
// (電池電圧のフレームで区切られたときなど)、遅れがほぼ 0 の点を使える
static void fifoTiming(int64_t &firstUs, float &periodUs)
{
    const double T = s_periodUs;
    double c = 1e300;
    for (uint32_t k = 0; k <= s_n; k++) {
        const double v = (double)(s_baseUs + s_readUs[k]) - (double)(s_baseG + k) * T;
        if (v < c) c = v;
    }
    for (uint8_t i = 0; i < s_trkN; i++) {
        const double v = (double)s_trkT[i] - (double)s_trkG[i] * T;
        if (v < c) c = v;
    }
    if (s_trkG0 > 0) {                       // 作りかけのブロックの最小の点
        const double v = (double)s_blkT - (double)s_blkG * T;
        if (v < c) c = v;
    }
    // 水晶の時計から、記録の時計 (記録を始めてからの時間) へ
    const int64_t offUs = nowRelUs() - esp_timer_get_time();
    firstUs  = (int64_t)llround(c + (double)s_baseG * T) + offUs;
    periodUs = s_periodUs;
}

static void closeFrame()
{
    if (!s_open) return;
    int64_t firstUs;
    float   periodUs;
    fifoTiming(firstUs, periodUs);
    if (firstUs < 0) firstUs = 0;
    uint32_t firstMs = (uint32_t)((firstUs + 500) / 1000);
    if (s_prevEndOk && firstMs <= s_prevEndMs) firstMs = s_prevEndMs + 1;   // 時刻を戻さない
    const uint16_t per = (uint16_t)lroundf(periodUs);
    s_prevEndMs = fifoRel(firstMs, per, s_n);
    s_prevEndOk = true;
    s_frame[2] = s_n;
    put32(s_frame + 4, firstMs);
    put16(s_frame + 13, s_len);
    put16(s_frame + 15, per);
    writeFrame(s_frame, FIFO_HDR + s_len, s_n + 1);
    s_open = false;
}

// サンプルの符号 (TAG_FIFO)。d = 気圧の差。書いたバイト数を返す (詰められなければ 0)
//   0xxxxxxx                    : d = 7 ビット (-64〜63)
//   10dddddd dddddddd           : d = 14 ビット (-8192〜8191)
//   0xC0, d (int16)             : そのほか
//   0xC1, 温度の差 (int8)       : 温度 [0.01 ℃] を変える (後に続くサンプルの値)
static uint32_t fifoCode(uint8_t *p, int32_t d)
{
    if (d >= -64 && d <= 63) {
        p[0] = (uint8_t)(d & 0x7F);
        return 1;
    }
    if (d >= -8192 && d <= 8191) {
        p[0] = (uint8_t)(0x80 | ((d >> 8) & 0x3F));
        p[1] = (uint8_t)d;
        return 2;
    }
    if (d >= -32768 && d <= 32767) {
        p[0] = 0xC0;
        put16(p + 1, (uint16_t)(int16_t)d);
        return 3;
    }
    return 0;
}

// fifoCode の逆。温度の符号 (0xC1) は、読み飛ばして temp100 に足す。
// 読んだバイト数を返す (壊れていれば 0)
static uint32_t unfifoCode(const uint8_t *p, uint32_t left, int32_t &d, int16_t &temp100)
{
    uint32_t used = 0;
    while (left - used >= 2 && p[used] == 0xC1) {
        temp100 = (int16_t)(temp100 + (int8_t)p[used + 1]);
        used += 2;
    }
    if (used >= left) return 0;
    const uint8_t b = p[used];
    if (b < 0x80) {
        d = (b & 0x40) ? (int32_t)b - 128 : b;
        return used + 1;
    }
    if (b < 0xC0) {
        if (left - used < 2) return 0;
        const int32_t v = ((b & 0x3F) << 8) | p[used + 1];
        d = v >= 8192 ? v - 16384 : v;
        return used + 2;
    }
    if (b == 0xC0) {
        if (left - used < 3) return 0;
        d = (int16_t)get16(p + used + 1);
        return used + 3;
    }
    return 0;
}


// TAG_PACK (版 5) のサンプルの符号を読む。dt = 前のサンプルからの時間 [ms]、d = 気圧の差。
// 温度の符号 (0xC1) は、読み飛ばして temp100 に足す。
// 読んだバイト数を返す (壊れていれば 0)
static uint32_t unpackSample(const uint8_t *p, uint32_t left, uint32_t &dt, int32_t &d, int16_t &temp100)
{
    uint32_t used = 0;
    while (left - used >= 2 && p[used] == 0xC1) {
        temp100 = (int16_t)(temp100 + (int8_t)p[used + 1]);
        used += 2;
    }
    if (used >= left) return 0;
    const uint8_t b = p[used];
    if (b < 0x80) {
        dt = PACK_DT;
        d  = (b & 0x40) ? (int32_t)b - 128 : b;
        return used + 1;
    }
    if (b < 0xC0) {
        if (left - used < 2) return 0;
        const int32_t v = ((b & 7) << 8) | p[used + 1];
        dt = 32 + ((b >> 3) & 7);
        d  = v >= 1024 ? v - 2048 : v;
        return used + 2;
    }
    if (b == 0xC0) {
        if (left - used < 4) return 0;
        dt = p[used + 1];
        d  = (int16_t)get16(p + used + 2);
        return used + 4;
    }
    return 0;
}

// 記録の間隔 [ms]。0 = 毎サンプル (30Hz)。版 3 までの記録は 0
static uint32_t intervalOf(const LogHeader &h)
{
    if (h.version < 4 || h.intervalMs == 0xFFFFFFFFUL) return 0;
    return h.intervalMs;
}

static void clearAvg()
{
    s_accN = 0; s_accRaw = 0; s_accHpf = 0; s_accTemp = 0; s_accRel = 0;
}

// 平均の途中を、1 つのサンプルとして書く。時刻は、平均したサンプルの時刻の平均。
// 入りきらなければ書かずに false
static bool flushAvg()
{
    if (s_accN == 0) return true;
    if (s_end + AVG_FRAME + BOOT_FRAME > s_part->size) return false;
    const int64_t n = s_accN;
    int64_t hpf = (s_accHpf >= 0 ? s_accHpf + n / 2 : s_accHpf - n / 2) / n;
    if (hpf >  0x7FFFFF) hpf =  0x7FFFFF;
    if (hpf < -0x7FFFFF) hpf = -0x7FFFFF;
    const int16_t  t100 = (int16_t)((s_accTemp >= 0 ? s_accTemp + n / 2 : s_accTemp - n / 2) / n);
    const uint32_t rel  = (uint32_t)(s_accRel / (uint64_t)n);
    const int32_t  raw  = (int32_t)((s_accRaw + n / 2) / n);

    // 前の平均との差が小さければ、差を 8 バイトで書く (TAG_AVGD)。
    // 時刻は「前 + 記録の間隔」からのずれ、温度は差、変化量はそのまま (16 ビット)
    const int64_t dRel  = (int64_t)rel - s_avgRel - intervalOf(s_header);
    const int32_t dRaw  = raw - s_avgRaw;
    const int32_t dTemp = t100 - s_avgTemp;
    if (s_avgHave && dRel >= -128 && dRel <= 127 && dRaw >= -32768 && dRaw <= 32767 &&
        dTemp >= -128 && dTemp <= 127 && hpf >= -32768 && hpf <= 32767) {
        uint8_t f[AVGD_FRAME];
        f[0] = TAG_AVGD;
        f[1] = s_header.session;
        f[2] = (uint8_t)(int8_t)dRel;
        put16(f + 3, (uint16_t)(int16_t)dRaw);
        f[5] = (uint8_t)(int8_t)dTemp;
        put16(f + 6, (uint16_t)(int16_t)hpf);
        writeFrame(f, AVGD_FRAME, 1);
    } else {
        uint8_t f[AVG_FRAME];
        f[0] = TAG_AVG;
        f[1] = s_header.session;
        put32(f + 2, rel);
        put24(f + 6, (uint32_t)raw);
        put16(f + 9, (uint16_t)t100);
        put24(f + 11, (uint32_t)hpf & 0xFFFFFF);
        writeFrame(f, AVG_FRAME, 1);
    }
    s_avgHave = true;
    s_avgRel  = rel;
    s_avgRaw  = raw;
    s_avgTemp = t100;
    clearAvg();
    return true;
}

// ---------------------------------------------------------------- record

static void stopRecording(int64_t phoneMs)
{
    if (!s_recording) return;
    closeFrame();
    flushAvg();                     // 途中の平均も残す (入りきらなければ捨てる)
    clearAvg();
    writeStop(phoneMs);
    s_recording = false;
    s_rtcMagic  = 0;
    Serial.printf("# log: recording stopped (%lu samples%s)\n", (unsigned long)s_count,
                  phoneMs > 0 ? ", time corrected" : "");
}

static void startRecording(int64_t epochMs, int32_t tzMin, bool wifiOff, uint32_t intervalMs)
{
    if (s_recording) stopRecording(0);

    // 基板の時計を、スマホの時計に合わせる (再起動しても、電源が切れるまで進み続ける)
    struct timeval tv;
    tv.tv_sec  = (time_t)(epochMs / 1000);
    tv.tv_usec = (suseconds_t)((epochMs % 1000) * 1000);
    settimeofday(&tv, nullptr);

    const uint8_t session = s_valid ? (uint8_t)(s_header.session + 1) : (uint8_t)esp_random();
    memset(&s_header, 0xFF, sizeof(s_header));
    memcpy(s_header.magic, "PMLG", 4);
    s_header.version      = LOG_VERSION;
    s_header.session      = session;
    s_header.tzMin        = (int16_t)tzMin;
    s_header.startEpochMs = epochMs;
    const HighPassFilter::State st = s_hpf ? s_hpf->getState() : HighPassFilter::State{ 0, 0, false };
    s_header.hpfPrevX = st.prevX;
    s_header.hpfY     = st.y;
    s_header.hpfInit  = st.init ? 1 : 0;
    s_header.mode     = (uint8_t)g_set.mode;
    s_header.role     = (uint8_t)g_set.role;
    s_header.flags    = wifiOff ? FLAG_WIFI_OFF : 0;
    strlcpy(s_header.name, settingsNodeName().c_str(), sizeof(s_header.name));
    s_header.intervalMs = intervalMs;

    flashPrepare();
    esp_partition_erase_range(s_part, 0, SECTOR);
    esp_partition_write(s_part, 0, &s_header, sizeof(s_header));
    memset(&s_stop, 0xFF, sizeof(s_stop));
    s_valid     = true;
    s_end       = DATA_START;
    s_erasedTo  = DATA_START;               // 記録の領域は、書く直前に消去する
    s_count     = 0;
    s_open      = false;
    s_lastRel   = 0;
    s_battMv    = UINT32_MAX;
    clearAvg();
    s_nextRel   = 0;
    s_avgHave   = false;
    s_gap       = true;
    s_prevEndOk = false;
    s_full      = false;
    s_recording = true;
    s_rtcMagic  = RTC_MAGIC ^ session;
    Serial.printf("# log: recording started (session %u, role %s, interval %lu ms)\n", session,
                  linkRoleName(s_header.role), (unsigned long)intervalMs);
}

static void clearRecording()
{
    s_recording = false;
    s_open  = false;
    s_count = 0;
    s_full  = false;
    s_rtcMagic = 0;
    flashPrepare();
    esp_partition_erase_range(s_part, 0, SECTOR);
    s_valid = false;
    s_end   = DATA_START;
    Serial.println(F("# log: cleared"));
}

// 電池電圧のフレーム。値が変わったときだけ書く (測るのは 1 分ごと)。
// 後に続くサンプルの値になるので、作りかけのフレームを先に閉じる
static void writeBattery(uint32_t mv)
{
    if (mv > 0xFFFF) mv = 0xFFFF;
    if (mv == s_battMv) return;
    if (s_end + BATT_FRAME + FIFO_FRAME_MAX + BOOT_FRAME > s_part->size) return;   // いっぱいになる直前
    closeFrame();
    uint8_t b[BATT_FRAME];
    b[0] = TAG_BATT;
    b[1] = s_header.session;
    put16(b + 2, mv);
    writeFrame(b, BATT_FRAME, 0);
    s_battMv = mv;
}

void devlogPush(int32_t raw, int32_t shift, float tempC, uint32_t batteryMv, int32_t hpf, uint8_t age)
{
    // センサの測定の間隔は、記録していないときも求め続ける (記録を始めたときから、正しい時刻にする)
    const int32_t ageUs = (int32_t)lroundf(age * s_periodUs);
    const int64_t tUs   = esp_timer_get_time() - ageUs;   // 読んだ時刻 (水晶の時計)
    periodTrack(tUs);
    if (!s_recording) return;
    writeBattery(batteryMv);
    // 読んだ時刻。同じときに読んだ値のうち、古いものは、センサの間隔の分だけ前に測ったとする
    const int64_t readUs = nowRelUs() - ageUs;
    uint32_t rel = (uint32_t)(readUs > 0 ? readUs / 1000 : 0);
    if (rel < s_lastRel) rel = s_lastRel;

    // 記録の間隔が 1 秒以上: 間隔ごとに平均して、1 つのサンプルにする。
    // 区切りは、記録を始めてからの時間で、間隔の倍数 (再起動したあとも同じ)
    const uint32_t iv = intervalOf(s_header);
    if (iv > 0) {
        if (s_accN > 0 && rel >= s_nextRel) {
            if (!flushAvg()) {
                clearAvg();
                stopRecording(0);
                s_full = true;
                Serial.println(F("# log: full"));
                return;
            }
        }
        if (s_accN == 0) s_nextRel = (rel / iv + 1) * iv;
        s_accN++;
        s_accRaw  += raw;
        s_accHpf  += hpf;
        s_accTemp += lroundf(tempC * 100.0f);
        s_accRel  += rel;
        s_lastRel  = rel;
        return;
    }

    // 毎サンプル (センサの間隔): 気圧の差だけを書く。時刻は、フレームを閉じるときに、
    // 読んだ時刻から求める (fifoTiming)
    // 記録を始めた直後に読んだ値は、始める前に測ったものかもしれない (時刻がマイナスになる)。
    // 読むまでの遅れは最大 約 35ms なので、始めてから 60ms までに読んだ値 (最大 2 個) は記録しない
    if (!s_open && s_count == 0 && readUs < 60000) return;
    const int16_t t100 = (int16_t)lroundf(tempC * 100.0f);
    if (s_open) {
        const int32_t rUs = (int32_t)(tUs - s_baseUs);
        const uint32_t k  = s_n + 1;
        // 見込みより大きく遅れた: サンプルが抜けた (基板が 1 秒以上止まった、など)
        const bool late = rUs - (s_envUs + (int32_t)lroundf(k * s_periodUs)) > FIFO_GAP_US;
        // 温度は、変わっていれば、約 1 秒ごとに書き直す
        uint8_t  code[5];
        uint32_t c  = 0;
        bool     ok = !s_gap && !late && s_n < FIFO_MAX && shift == s_shift;
        if (ok && t100 != s_temp && s_tempAge >= FIFO_TEMP_EVERY) {
            const int32_t dT = t100 - s_temp;
            if (dT >= -128 && dT <= 127) {
                code[0] = 0xC1;
                code[1] = (uint8_t)(int8_t)dT;
                c = 2;
            } else {
                ok = false;                     // 大きく変わった: 新しいフレームにする
            }
        }
        if (ok) {
            const uint32_t m = fifoCode(code + c, raw - s_lastRaw);
            if (m > 0 && s_len + c + m <= FIFO_CODES_MAX) {
                memcpy(s_frame + FIFO_HDR + s_len, code, c + m);
                s_len += c + m;
                s_readUs[k] = rUs;
                const int32_t env = rUs - (int32_t)lroundf(k * s_periodUs);
                if (env < s_envUs) s_envUs = env;
                s_n++;
                if (c > 0) { s_temp = t100; s_tempAge = 0; }
                s_tempAge++;
                s_lastRel = rel;
                s_lastRaw = raw;
                if (s_n >= FIFO_MAX) closeFrame();
                return;
            }
        }
        closeFrame();
    }

    // 新しいフレーム。入りきらなければ、いっぱいで止める
    if (s_end + FIFO_FRAME_MAX + BOOT_FRAME > s_part->size) {
        stopRecording(0);
        s_full = true;
        Serial.println(F("# log: full"));
        return;
    }
    s_frame[0] = TAG_FIFO;
    s_frame[1] = s_header.session;
    s_frame[2] = 0;
    s_frame[3] = (uint8_t)shift;
    put24(s_frame + 8, (uint32_t)raw);
    put16(s_frame + 11, (uint16_t)t100);     // 時刻 (4) と、符号の長さ・間隔 (13、15) は閉じるときに書く
    s_open      = true;
    s_gap       = false;
    s_n         = 0;
    s_len       = 0;
    s_temp      = t100;
    s_tempAge   = 0;
    s_shift     = shift;
    s_baseUs    = tUs;
    s_baseG     = s_trkG0 - 1;              // periodTrack で数えた、このサンプルの番号
    s_readUs[0] = 0;
    s_envUs     = 0;
    s_lastRel   = rel;
    s_lastRaw   = raw;
}

void devlogMarkGap()
{
    s_gap = true;
    periodTrackReset();
}

void devlogFlush()
{
    if (s_recording) closeFrame();
}

// ---------------------------------------------------------------- begin / service

void devlogBegin(HighPassFilter *hpf)
{
    s_hpf  = hpf;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!s_part) {
        Serial.println(F("# log: partition not found (Partition Scheme: No OTA?)"));
        return;
    }

    esp_partition_read(s_part, 0, &s_header, sizeof(s_header));
    s_valid = memcmp(s_header.magic, "PMLG", 4) == 0 &&
              (s_header.version >= 2 && s_header.version <= LOG_VERSION);
    if (!s_valid) {
        Serial.printf("# log: empty (%lu KB)\n", (unsigned long)(s_part->size / 1024));
        return;
    }
    esp_partition_read(s_part, STOP_OFFSET, &s_stop, sizeof(s_stop));

    // フレームをたどって、記録の終わりを探す
    uint32_t off = DATA_START, count = 0, lastRel = 0;
    uint8_t  h[FIFO_HDR];
    while (off + BOOT_FRAME <= s_part->size) {
        esp_partition_read(s_part, off, h, 8);
        if (h[1] != s_header.session) break;
        if (h[0] == TAG_FIFO) {
            if (off + FIFO_HDR > s_part->size) break;
            esp_partition_read(s_part, off, h, FIFO_HDR);
            const uint32_t blen = get16(h + 13);
            if (h[2] > FIFO_MAX || blen > FIFO_CODES_MAX || off + FIFO_HDR + blen > s_part->size) break;
            uint8_t c[FIFO_CODES_MAX];               // 符号が壊れていないか確かめる
            esp_partition_read(s_part, off + FIFO_HDR, c, blen);
            uint32_t pos = 0;
            int16_t  temp = 0;
            bool     bad = false;
            for (uint8_t i = 0; i < h[2]; i++) {
                int32_t d;
                const uint32_t used = unfifoCode(c + pos, blen - pos, d, temp);
                if (used == 0) { bad = true; break; }
                pos += used;
            }
            if (bad) break;
            lastRel = fifoRel(get32(h + 4), get16(h + 15), h[2]);
            count += h[2] + 1;
            off += FIFO_HDR + blen;
        } else if (h[0] == TAG_PACK) {
            if (off + PACK_HDR > s_part->size) break;
            esp_partition_read(s_part, off, h, PACK_HDR);
            const uint32_t blen = get16(h + 13);
            if (h[2] > PACK_MAX || blen > PACK_CODES_MAX || off + PACK_HDR + blen > s_part->size) break;
            uint8_t c[PACK_CODES_MAX];               // 最後のサンプルの rel を求める
            esp_partition_read(s_part, off + PACK_HDR, c, blen);
            uint32_t rel = get32(h + 4), pos = 0;
            int16_t  temp = 0;
            bool     bad = false;
            for (uint8_t i = 0; i < h[2]; i++) {
                uint32_t dt; int32_t d;
                const uint32_t used = unpackSample(c + pos, blen - pos, dt, d, temp);
                if (used == 0) { bad = true; break; }
                pos += used;
                rel += dt;
            }
            if (bad) break;
            lastRel = rel;
            count += h[2] + 1;
            off += PACK_HDR + blen;
        } else if (h[0] == TAG_FRAME) {
            if (h[2] > MAX_DELTAS) break;
            const uint32_t len = FRAME_HDR + h[2] * 3;
            if (off + len > s_part->size) break;
            lastRel = get32(h + 4);
            if (h[2] > 0) {                        // 最後のサンプルの rel を求める
                uint8_t d[MAX_DELTAS * 3];
                esp_partition_read(s_part, off + FRAME_HDR, d, h[2] * 3);
                for (uint8_t i = 0; i < h[2]; i++) lastRel += d[i * 3];
            }
            count += h[2] + 1;
            off += len;
        } else if (h[0] == TAG_BOOT) {
            off += BOOT_FRAME;
        } else if (h[0] == TAG_BATT) {
            off += BATT_FRAME;
        } else if (h[0] == TAG_AVG) {
            if (off + AVG_FRAME > s_part->size) break;
            lastRel = get32(h + 2);
            count += 1;
            off += AVG_FRAME;
        } else if (h[0] == TAG_AVGD) {
            if (off + AVGD_FRAME > s_part->size) break;
            lastRel += (int8_t)h[2] + intervalOf(s_header);
            count += 1;
            off += AVGD_FRAME;
        } else {
            break;
        }
    }
    s_end       = off;
    s_count     = count;
    s_lastRel   = lastRel;
    s_erasedTo  = (off + SECTOR - 1) / SECTOR * SECTOR;   // 書きかけのセクタは、消去済み

    // いっぱいで止まった記録か (残りに、次のフレームが入らない)。
    // 止まった理由は保存していないので、残りの大きさから判定する (前の版で記録したものにも効く)
    const uint32_t frameMax = s_header.version >= 6 ? FIFO_FRAME_MAX :
                              s_header.version >= 5 ? PACK_FRAME_MAX : FRAME_MAX;
    s_full = (s_stop.stopped == 0) && (s_end + frameMax + BOOT_FRAME > s_part->size);

    // 記録の途中で再起動した: 時計が続いていれば、続きから記録する
    if (s_stop.stopped == 0xFF) {
        const esp_reset_reason_t why = esp_reset_reason();
        const bool timeOk = s_rtcMagic == (RTC_MAGIC ^ s_header.session) &&
                            why != ESP_RST_POWERON && why != ESP_RST_BROWNOUT &&
                            nowEpochMs() - s_header.startEpochMs >= (int64_t)lastRel;
        if (timeOk && s_end + BOOT_FRAME <= s_part->size) {
            uint8_t b[BOOT_FRAME];
            b[0] = TAG_BOOT;
            b[1] = s_header.session;
            put32(b + 2, nowRel());
            writeFrame(b, BOOT_FRAME, 0);
            s_recording = true;
            Serial.printf("# log: recording resumed (%lu samples)\n", (unsigned long)s_count);
        } else {
            // 電源が切れて、時計が失われた。ここで記録を終える
            s_recording = true;              // writeStop のため、一時的に
            writeStop(0);
            s_recording = false;
            Serial.printf("# log: recording ended by power loss (%lu samples)\n", (unsigned long)s_count);
        }
    } else {
        Serial.printf("# log: %lu samples\n", (unsigned long)s_count);
    }
}

void devlogService(uint32_t nowMs)
{
    if (!s_part) return;
    const Cmd cmd = s_cmd;
    if (cmd != CMD_NONE) {
        s_cmd = CMD_NONE;
        if (cmd == CMD_START) {
            startRecording(s_cmdEpoch, s_cmdTz, s_cmdWifiOff, s_cmdInterval);
            if (s_cmdWifiOff) s_rebootAtMs = nowMs + 800;   // 応答を返してから再起動する
        } else if (cmd == CMD_STOP) {
            stopRecording(s_cmdEpoch);
        } else if (cmd == CMD_CLEAR) {
            clearRecording();
        }
    }
    if (s_rebootAtMs && (int32_t)(nowMs - s_rebootAtMs) >= 0) {
        s_rebootAtMs = 0;
        Serial.println(F("# log: Wi-Fi off, recording continues in normal mode"));
        bootRebootInto(false);
    }
}

void devlogRequestStart(int64_t epochMs, int32_t tzMin, bool wifiOff, uint32_t intervalMs)
{
    Serial.printf("# log: start requested%s\n", wifiOff ? " (Wi-Fi off)" : "");
    s_cmdEpoch   = epochMs;
    s_cmdTz      = tzMin;
    s_cmdWifiOff = wifiOff;
    s_cmdInterval = intervalMs;
    s_cmd        = CMD_START;
}
void devlogRequestStop(int64_t epochMs) { s_cmdEpoch = epochMs; s_cmd = CMD_STOP; }
void devlogRequestClear()               { s_cmd = CMD_CLEAR; }

bool     devlogAvailable() { return s_part != nullptr; }
bool     devlogRecording() { return s_recording; }
bool     devlogFull()      { return s_full; }
uint32_t devlogCount()     { return s_count + (s_open ? s_n + 1 : 0); }
uint32_t devlogCapacity()
{
    if (!s_part) return 0;
    const uint32_t rest = s_part->size > s_end ? s_part->size - s_end : 0;
    if (s_valid && s_header.version < 5) {           // 前の形式の記録
        if (devlogIntervalMs() > 0) return devlogCount() + rest / AVG_FRAME;   // 平均 1 つで 14 バイト
        return devlogCount() + rest * 30 / 100;      // 30 サンプルで約 100 バイト
    }
    if (devlogIntervalMs() > 0) return devlogCount() + rest / AVGD_FRAME;      // 平均 1 つで 8 バイト
    // 1 サンプルのバイト数は、変化の大きさで変わる (約 1.1。大きく変わり続けると、もっと大きい)。
    // 記録が少ないうちは 1.1 とし、あとは、ここまでの実績から見込む
    const uint32_t used = s_end - DATA_START;
    if (s_count < 3000 || used == 0) return devlogCount() + (uint32_t)((uint64_t)rest * 10 / 11);
    return devlogCount() + (uint32_t)((uint64_t)rest * s_count / used);
}

uint32_t devlogIntervalMs()
{
    return s_valid ? intervalOf(s_header) : 0;
}

// ---------------------------------------------------------------- CSV
//
// 行の長さをそろえて (固定長)、ファイルの大きさを、作り始める前に決める。
// 大きさ (Content-Length) が分かると、スマホが保存の進み具合を表示できる。
// また、ESP32-C3 には浮動小数点の演算器がないので、数値は整数だけで文字にする (速い)。
//   1 行目: # で始まるコメント (装置の名前、役割など。csvComment)
//   2 行目: 列の名前
//   3 行目から: データ
//   time (23) , unix_ms (13) , elapsed_ms (8) , pressure_hPa (9) , raw (8) , hpf (8) ,
//   temperature_C (6) , battery_mV (4) \r\n
//   例: 2026-10-18 12:34:56.789,1792310863620,00012345,1024.3348,06555743,-0000123,+27.34,2512
// 版 2 の記録には電池電圧がないので、battery_mV は空欄にする (1 行 84 バイト)
static const char  CSV_COLUMNS[] =
    "time,unix_ms,elapsed_ms,pressure_hPa,raw,hpf,temperature_C,battery_mV\r\n";
static const size_t CSV_LINE_LEN    = 88;
static const size_t CSV_LINE_LEN_V2 = 84;

// ダウンロードの進み具合 (表示ページへ送る)
static volatile uint32_t s_dlSent   = 0;
static volatile uint32_t s_dlTotal  = 0;
static volatile uint32_t s_dlLastMs = 0;

struct CsvState {
    LogHeader h;
    StopInfo  stop;
    uint32_t  off = DATA_START, end = DATA_START;
    uint32_t  lines = 0, linesDone = 0;    // 出す行数と、出した行数
    HighPassFilter hpf;
    uint8_t   frame[READ_FRAME_MAX];
    uint8_t   n = 0, idx = 0;          // 読んでいるフレームの差分の数と、次のサンプル
    bool      inFrame = false;
    bool      pack = false;                // TAG_PACK のフレーム (符号は frame + PACK_HDR から)
    bool      fifo = false;                // TAG_FIFO のフレーム (符号は frame + FIFO_HDR から)
    uint32_t  pos = 0, blen = 0;           // TAG_PACK、TAG_FIFO: 次に読む符号の位置と、符号のバイト数
    uint32_t  rel0 = 0, period = 0;        // TAG_FIFO: 先頭の時刻 [ms] と、間隔 [µs]
    uint32_t  rel = 0;
    int32_t   raw = 0, shift = 8;
    int16_t   temp100 = 0;
    bool      avg = false;                 // 平均したサンプル (TAG_AVG、TAG_AVGD)。変化量は avgHpf
    bool      avgHave = false;             // 前の平均がある (TAG_AVGD は、その差)
    int32_t   avgHpf = 0;
    uint32_t  battMv = 0;
    size_t    lineBytes = CSV_LINE_LEN;    // データの 1 行の長さ (版 2 は CSV_LINE_LEN_V2)
    char      line[CSV_LINE_LEN + 8];
    size_t    lineLen = 0, linePos = 0;
    String    head;                        // BOM、コメント、列の名前
    size_t    headPos = 0;                 // head を送った長さ
    int64_t   cachedSec = INT64_MIN;   // 日時の文字列は、秒が変わったときだけ作る
    char      dateStr[24];
};

// 次のサンプルを読む。終わりなら false
static bool nextSample(CsvState *st)
{
    while (!st->inFrame || st->idx > st->n) {
        st->inFrame = false;
        if (st->off + BOOT_FRAME > st->end) return false;
        uint8_t h[2];
        esp_partition_read(s_part, st->off, h, 2);
        if (h[1] != st->h.session) return false;
        if (h[0] == TAG_BOOT) {                  // 再起動: HPF は初めから
            st->hpf.reset();
            st->off += BOOT_FRAME;
            continue;
        }
        if (h[0] == TAG_BATT) {                  // 電池電圧: 後に続くサンプルの値
            uint8_t b[BATT_FRAME];
            esp_partition_read(s_part, st->off, b, BATT_FRAME);
            st->battMv = get16(b + 2);
            st->off += BATT_FRAME;
            continue;
        }
        if (h[0] == TAG_AVG) {                   // 平均したサンプル (1 つだけ)
            if (st->off + AVG_FRAME > st->end) return false;
            uint8_t f[AVG_FRAME];
            esp_partition_read(s_part, st->off, f, AVG_FRAME);
            st->off    += AVG_FRAME;
            st->rel     = get32(f + 2);
            st->raw     = (int32_t)get24(f + 6);
            st->temp100 = (int16_t)get16(f + 9);
            st->avgHpf  = ((int32_t)(get24(f + 11) << 8)) >> 8;   // 24 ビットの符号を広げる
            st->avg     = true;
            st->avgHave = true;
            st->pack    = false;
            st->fifo    = false;
            st->n       = 0;
            st->idx     = 0;
            st->inFrame = true;
            continue;
        }
        if (h[0] == TAG_AVGD) {                  // 平均したサンプル (前の平均との差)
            if (!st->avgHave || st->off + AVGD_FRAME > st->end) return false;
            uint8_t f[AVGD_FRAME];
            esp_partition_read(s_part, st->off, f, AVGD_FRAME);
            st->off    += AVGD_FRAME;
            st->rel    += (int8_t)f[2] + intervalOf(st->h);
            st->raw    += (int16_t)get16(f + 3);
            st->temp100 = (int16_t)(st->temp100 + (int8_t)f[5]);
            st->avgHpf  = (int16_t)get16(f + 6);
            st->avg     = true;
            st->pack    = false;
            st->fifo    = false;
            st->n       = 0;
            st->idx     = 0;
            st->inFrame = true;
            continue;
        }
        if (h[0] == TAG_FIFO) {
            if (st->off + FIFO_HDR > st->end) return false;
            esp_partition_read(s_part, st->off, st->frame, FIFO_HDR);
            st->n    = st->frame[2];
            st->blen = get16(st->frame + 13);
            if (st->n > FIFO_MAX || st->blen > FIFO_CODES_MAX) return false;
            if (st->off + FIFO_HDR + st->blen > st->end) return false;
            if (st->blen) esp_partition_read(s_part, st->off + FIFO_HDR, st->frame + FIFO_HDR, st->blen);
            st->off    += FIFO_HDR + st->blen;
            st->avg     = false;
            st->pack    = false;
            st->fifo    = true;
            st->pos     = 0;
            st->shift   = st->frame[3];
            st->rel0    = get32(st->frame + 4);
            st->period  = get16(st->frame + 15);
            st->rel     = st->rel0;
            st->raw     = (int32_t)get24(st->frame + 8);
            st->temp100 = (int16_t)get16(st->frame + 11);
            st->idx     = 0;
            st->inFrame = true;
            continue;
        }
        if (h[0] == TAG_PACK) {
            if (st->off + PACK_HDR > st->end) return false;
            esp_partition_read(s_part, st->off, st->frame, PACK_HDR);
            st->n    = st->frame[2];
            st->blen = get16(st->frame + 13);
            if (st->n > PACK_MAX || st->blen > PACK_CODES_MAX) return false;
            if (st->off + PACK_HDR + st->blen > st->end) return false;
            if (st->blen) esp_partition_read(s_part, st->off + PACK_HDR, st->frame + PACK_HDR, st->blen);
            st->off    += PACK_HDR + st->blen;
            st->avg     = false;
            st->pack    = true;
            st->fifo    = false;
            st->pos     = 0;
            st->shift   = st->frame[3];
            st->rel     = get32(st->frame + 4);
            st->raw     = (int32_t)get24(st->frame + 8);
            st->temp100 = (int16_t)get16(st->frame + 11);
            st->idx     = 0;
            st->inFrame = true;
            continue;
        }
        if (h[0] != TAG_FRAME) return false;
        st->avg  = false;
        st->pack = false;
        st->fifo = false;
        esp_partition_read(s_part, st->off, st->frame, FRAME_HDR);
        st->n = st->frame[2];
        if (st->n > MAX_DELTAS) return false;
        const uint32_t len = FRAME_HDR + st->n * 3;
        if (st->off + len > st->end) return false;
        if (st->n) esp_partition_read(s_part, st->off + FRAME_HDR, st->frame + FRAME_HDR, st->n * 3);
        st->off    += len;
        st->shift   = st->frame[3];
        st->rel     = get32(st->frame + 4);
        st->raw     = (int32_t)get24(st->frame + 8);
        st->temp100 = (int16_t)get16(st->frame + 11);
        st->idx     = 0;
        st->inFrame = true;
    }
    if (st->idx > 0 && st->fifo) {
        int32_t d;
        const uint32_t used = unfifoCode(st->frame + FIFO_HDR + st->pos, st->blen - st->pos, d, st->temp100);
        if (used == 0) return false;
        st->pos += used;
        st->rel  = fifoRel(st->rel0, st->period, st->idx);
        st->raw += d;
    } else if (st->idx > 0 && st->pack) {
        uint32_t dt; int32_t d;
        const uint32_t used = unpackSample(st->frame + PACK_HDR + st->pos, st->blen - st->pos, dt, d, st->temp100);
        if (used == 0) return false;
        st->pos += used;
        st->rel += dt;
        st->raw += d;
    } else if (st->idx > 0) {
        const uint8_t *p = st->frame + FRAME_HDR + (st->idx - 1) * 3;
        st->rel += p[0];
        st->raw += (int16_t)get16(p + 1);
    }
    st->idx++;
    return true;
}

static int32_t clampAbs(int32_t v, int32_t lim) { return v > lim ? lim : (v < -lim ? -lim : v); }

// 行の長さを st->lineBytes にそろえて、改行で終える (len は、改行の前までに書いた長さ)
static void padLine(CsvState *st, int len)
{
    const int n = (int)st->lineBytes;
    if (len < 0) len = 0;
    if (len > n - 2) len = n - 2;
    for (int i = len; i < n - 2; i++) st->line[i] = ' ';
    st->line[n - 2] = '\r';
    st->line[n - 1] = '\n';
    st->lineLen = st->lineBytes;
    st->linePos = 0;
    st->linesDone++;
}

// 次の 1 行を st->line に作る (必ず st->lineBytes 文字)。終わりなら false
static bool makeLine(CsvState *st)
{
    if (st->linesDone >= st->lines) return false;
    if (!nextSample(st)) {
        // 数えた行数より、読めたサンプルが少ない (起きないはず)。大きさを守るため、空の行で埋める
        padLine(st, 0);
        return true;
    }

    // 変化量: 毎サンプルの記録は、基板と同じ HPF で、気圧から計算し直す。
    // 平均した記録は、基板の HPF の出力を、間隔の中で平均した値 (記録してある)
    const int32_t hpf = st->avg ? st->avgHpf : st->hpf.update(st->raw, st->shift);

    // 時刻: 開始時のスマホの時計 + 経過時間。停止時のスマホの時計があれば、ずれを直線で補正する
    int64_t unixMs = st->h.startEpochMs + st->rel;
    if (st->stop.stopped == 0 && st->stop.stopPhoneMs > 0 && st->stop.stopRelMs > 0) {
        const int64_t err = st->stop.stopPhoneMs - (st->h.startEpochMs + (int64_t)st->stop.stopRelMs);
        unixMs += err * (int64_t)st->rel / (int64_t)st->stop.stopRelMs;
    }
    const int64_t localMs = unixMs - (int64_t)st->h.tzMin * 60000;
    const int64_t sec     = localMs / 1000;
    if (sec != st->cachedSec) {
        st->cachedSec = sec;
        const time_t t = (time_t)sec;
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(st->dateStr, sizeof(st->dateStr), "%04d-%02d-%02d %02d:%02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    // 気圧 [hPa] = raw / 6400。小数 4 桁を、整数で丸めて求める (raw x 25 / 16 = hPa x 10000)
    const int64_t p10k  = ((int64_t)st->raw * 25 + 8) / 16;
    const int32_t hpfC  = clampAbs(hpf, 9999999);
    const int32_t t100  = clampAbs(st->temp100, 9999);
    const uint32_t relC = st->rel > 99999999UL ? 99999999UL : st->rel;
    const uint32_t mvC  = st->battMv > 9999 ? 9999 : st->battMv;

    const size_t cap = sizeof(st->line);
    int len = snprintf(st->line, cap,
        "%s.%03d,%013lld,%08lu,%04ld.%04ld,%08ld,%c%07ld,%c%02d.%02d,",
        st->dateStr, (int)(localMs % 1000), (long long)unixMs, (unsigned long)relC,
        (long)(p10k / 10000) % 10000, (long)(p10k % 10000), (long)st->raw,
        hpfC < 0 ? '-' : '+', (long)(hpfC < 0 ? -hpfC : hpfC),
        t100 < 0 ? '-' : '+', (int)((t100 < 0 ? -t100 : t100) / 100), (int)((t100 < 0 ? -t100 : t100) % 100));
    if (st->h.version >= 3 && len > 0 && len < (int)cap) {
        len += snprintf(st->line + len, cap - len, "%04lu", (unsigned long)mvC);   // 版 2 は空欄
    }
    // 想定外の桁数 (起きないはず) でも、大きさを守るため、行の長さをそろえる
    padLine(st, len);
    return true;
}

// ファイル名に使えない文字 (/ \ : * ? " < > | と制御文字) を、_ にする
static String safeFileName(const String &in)
{
    String out;
    for (size_t i = 0; i < in.length() && out.length() < 120; i++) {
        const char c = in[i];
        if ((uint8_t)c < 0x20 || strchr("/\\:*?\"<>|", c)) out += '_';
        else out += c;
    }
    return out;
}

// /log/<ファイル名> のファイル名 (なければ、古い形の ?name=、それもなければ既定の名前)
static String fileNameFromRequest(AsyncWebServerRequest *req)
{
    String name;
    const String &url = req->url();                    // 受け取った時点で、% の表記は戻っている
    if (url.startsWith("/log/")) name = url.substring(5);
    else if (req->hasParam("name")) name = req->getParam("name")->value();
    name = safeFileName(name);
    if (name.length() == 0) name = "pressure-monitor.csv";
    return name;
}

// filename* 用に、UTF-8 のバイトを %XX にする (英数字と - _ . ~ はそのまま)
static String percentEncode(const String &s)
{
    static const char HEX_DIGITS[] = "0123456789ABCDEF";
    String out;
    for (size_t i = 0; i < s.length(); i++) {
        const uint8_t c = (uint8_t)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += HEX_DIGITS[c >> 4];
            out += HEX_DIGITS[c & 15];
        }
    }
    return out;
}

// コメントの値に、区切りの文字 (, と ") と改行が入らないようにする
static String commentValue(const char *in)
{
    String out;
    for (const char *p = in; *p; p++) {
        const char c = *p;
        out += ((uint8_t)c < 0x20 || c == ',' || c == '"') ? '_' : c;
    }
    return out;
}

// CSV の 1 行目 (# で始まるコメント)。記録した装置と、記録の条件。
// 「名前=値」をカンマで区切る (Excel では、セルに分かれて表示される)
//   # pressure-monitor log,name=ps-1a2b3c,mac=..,role=standalone,mode=1 標準,wifi=off,
//     start=2026-10-19 11:45:42 UTC+09:00,end=stopped,samples=553621,csv_firmware=2026-10-19
static String csvComment(const CsvState &st)
{
    const LogHeader &h = st.h;
    const bool v3 = h.version >= 3;
    String c = "# pressure-monitor log";

    char nameBuf[NODE_NAME_MAX + 1];
    if (v3) {
        memcpy(nameBuf, h.name, sizeof(nameBuf));
        nameBuf[NODE_NAME_MAX] = '\0';
    }
    c += ",name=";
    c += v3 ? commentValue(nameBuf) : String("unknown");

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[48];
    snprintf(buf, sizeof(buf), ",mac=%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    c += buf;

    c += ",role=";
    c += v3 ? linkRoleName(h.role) : "unknown";
    snprintf(buf, sizeof(buf), ",mode=%u ", (unsigned)h.mode + 1);
    c += buf;
    c += modeName(h.mode);
    c += ",wifi=";
    c += v3 ? ((h.flags & FLAG_WIFI_OFF) ? "off" : "on") : "unknown";

    // 記録の間隔。1 秒以上は、その間の平均 (気圧、温度、変化量)
    const uint32_t iv = intervalOf(h);
    if (iv == 0) {
        c += ",interval=1/30s";
    } else {
        snprintf(buf, sizeof(buf), ",interval=%lus (average)", (unsigned long)(iv / 1000));
        c += buf;
    }

    // 記録を始めた時刻 (スマホのタイムゾーン)。tzMin は Date.getTimezoneOffset() (JST は -540)
    const int32_t offMin = -(int32_t)h.tzMin;
    const time_t t = (time_t)((h.startEpochMs - (int64_t)h.tzMin * 60000) / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, sizeof(buf), ",start=%04d-%02d-%02d %02d:%02d:%02d UTC%c%02ld:%02ld",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
             offMin < 0 ? '-' : '+', (long)(abs(offMin) / 60), (long)(abs(offMin) % 60));
    c += buf;

    // 記録の終わり方。stopped = 「■ 停止」で止めた (時刻を補正した)、full = いっぱいで止まった、
    // power_loss = 電源が切れた、recording = 記録中
    const char *end = "recording";
    if (st.stop.stopped == 0) {
        end = st.stop.stopPhoneMs > 0 ? "stopped" : (s_full ? "full" : "power_loss");
    }
    c += ",end=";
    c += end;
    snprintf(buf, sizeof(buf), ",samples=%lu", (unsigned long)st.lines);
    c += buf;
    c += ",csv_firmware=" FIRMWARE_VERSION "\r\n";
    return c;
}

// CSV を作る準備 (書き終えたところまで)。先頭 (BOM、コメント、列の名前) も作る
static std::shared_ptr<CsvState> makeCsvState()
{
    auto st = std::make_shared<CsvState>();
    st->h    = s_header;
    st->stop = s_stop;
    portENTER_CRITICAL(&s_mux);            // 書き終えたところまで (位置とサンプル数を組で読む)
    st->end   = s_end;
    st->lines = s_count;
    portEXIT_CRITICAL(&s_mux);
    st->hpf.setState({ s_header.hpfPrevX, s_header.hpfY, s_header.hpfInit != 0 });
    st->lineBytes = st->h.version >= 3 ? CSV_LINE_LEN : CSV_LINE_LEN_V2;

    // 先頭: BOM、1 行目のコメント、2 行目の列の名前
    st->head  = "\xEF\xBB\xBF";
    st->head += csvComment(*st);
    st->head += CSV_COLUMNS;
    return st;
}

void devlogHandleCsv(AsyncWebServerRequest *req)
{
    if (!s_part || !s_valid) {
        req->send(404, "text/plain; charset=utf-8", "記録がありません");
        return;
    }
    auto st = makeCsvState();
    const size_t total = st->head.length() + (size_t)st->lines * st->lineBytes;
    s_dlSent   = 0;
    s_dlTotal  = total;
    s_dlLastMs = millis();
    Serial.printf("# log: CSV download started (%lu lines, %lu bytes)\n",
                  (unsigned long)st->lines, (unsigned long)total);

    // ファイル名は、ページが付ける。装置の名前 (日本語を含むことがある) を入れるため、
    // アドレスの最後 (/log/<ファイル名>) で受け取る。Android の Wi-Fi のログイン画面は、
    // ファイル名の見出し (filename*) を読まず、アドレスの最後をファイル名にするため。
    // ほかのブラウザ向けには、UTF-8 のファイル名を filename* で送る
    String name = fileNameFromRequest(req);

    AsyncWebServerResponse *res = req->beginResponse("text/csv; charset=utf-8", total,
        [st, total](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
            size_t n = 0;
            while (n < maxLen && st->headPos < st->head.length()) {   // 先頭 (BOM、コメント、列の名前)
                const size_t k = min(maxLen - n, st->head.length() - st->headPos);
                memcpy(buf + n, st->head.c_str() + st->headPos, k);
                st->headPos += k;
                n += k;
            }
            while (n < maxLen) {
                if (st->linePos >= st->lineLen && !makeLine(st.get())) break;
                const size_t k = min(maxLen - n, st->lineLen - st->linePos);
                memcpy(buf + n, st->line + st->linePos, k);
                st->linePos += k;
                n += k;
            }
            s_dlSent   = index + n;
            s_dlLastMs = millis();
            if (index + n >= total) Serial.println(F("# log: CSV download finished"));
            return n;
        });
    res->addHeader("Content-Disposition", "attachment; filename*=UTF-8''" + percentEncode(name));
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

// ---------------------------------------------------------------- 記録をそのまま送る
//
// 表示ページが、受け取った記録から、/log.csv と同じ CSV を作る (通信量は、1/30 秒の記録で約 1/70)。
// CSV の先頭 (コメントと列の名前) は、基板が作ったものを入れる (MAC アドレス、版などを含むため)
//   0  "PMLB"
//   4  形式 (1)
//   5  データの 1 行の長さ (88。版 2 の記録は 84)
//   6  予備 (2 バイト)
//   8  先頭の長さ (uint32)
//  12  行数 (uint32。コメントの samples と同じ)
//  16  記録の長さ (uint32。フラッシュの先頭からの位置で、書き終えたところまで)
//  20  先頭 (BOM、コメント、列の名前。UTF-8)
//  20 + 先頭の長さ: 記録 (ヘッダのセクタから、書き終えたところまで)
static const uint32_t BIN_PRE = 20;
static volatile uint32_t s_binSent   = 0;
static volatile uint32_t s_binTotal  = 0;
static volatile uint32_t s_binLastMs = 0;

void devlogHandleBin(AsyncWebServerRequest *req)
{
    if (!s_part || !s_valid) {
        req->send(404, "text/plain; charset=utf-8", "記録がありません");
        return;
    }
    auto st = makeCsvState();
    auto pre = std::make_shared<std::array<uint8_t, BIN_PRE>>();
    uint8_t *p = pre->data();
    memcpy(p, "PMLB", 4);
    p[4] = 1;
    p[5] = (uint8_t)st->lineBytes;
    p[6] = p[7] = 0;
    put32(p + 8,  st->head.length());
    put32(p + 12, st->lines);
    put32(p + 16, st->end);
    const size_t total = BIN_PRE + st->head.length() + st->end;
    s_binSent   = 0;
    s_binTotal  = total;
    s_binLastMs = millis();
    Serial.printf("# log: binary download started (%lu bytes)\n", (unsigned long)total);

    AsyncWebServerResponse *res = req->beginResponse("application/octet-stream", total,
        [st, pre, total](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
            size_t n = 0;
            const size_t headEnd = BIN_PRE + st->head.length();
            while (n < maxLen && index + n < total) {
                const size_t at = index + n;
                size_t k;
                if (at < BIN_PRE) {
                    k = min(maxLen - n, BIN_PRE - at);
                    memcpy(buf + n, pre->data() + at, k);
                } else if (at < headEnd) {
                    k = min(maxLen - n, headEnd - at);
                    memcpy(buf + n, st->head.c_str() + (at - BIN_PRE), k);
                } else {
                    k = min(maxLen - n, total - at);
                    esp_partition_read(s_part, at - headEnd, buf + n, k);
                }
                n += k;
            }
            s_binSent   = index + n;
            s_binLastMs = millis();
            if (index + n >= total) Serial.println(F("# log: binary download finished"));
            return n;
        });
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

bool devlogDownloading()
{
    // 送り終わっていない応答が、最近まで進んでいた
    const uint32_t now = millis();
    return (s_dlTotal > 0 && s_dlSent < s_dlTotal && (now - s_dlLastMs) < 60000) ||
           (s_binTotal > 0 && s_binSent < s_binTotal && (now - s_binLastMs) < 60000);
}

void devlogDownloadProgress(uint32_t &sent, uint32_t &total)
{
    sent  = s_dlSent;
    total = s_dlTotal;
}
