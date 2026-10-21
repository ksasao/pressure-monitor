#include "devlog.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_random.h>
#include <sys/time.h>
#include <time.h>
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
static const uint8_t  TAG_FRAME   = 0xA5;
static const uint8_t  TAG_BOOT    = 0xB0;
static const uint8_t  TAG_BATT    = 0xBA;
static const uint8_t  TAG_AVG     = 0xA7;              // 平均したサンプル (記録の間隔が 1 秒以上)
static const uint32_t FRAME_HDR   = 13;
static const uint8_t  MAX_DELTAS  = 29;              // 1 フレーム = 30 サンプル (約 1 秒)
static const uint32_t FRAME_MAX   = FRAME_HDR + MAX_DELTAS * 3;
static const uint32_t BOOT_FRAME  = 6;
static const uint32_t BATT_FRAME  = 4;
static const uint32_t AVG_FRAME   = 14;
static const uint32_t RTC_MAGIC   = 0x504D4C47;      // "PMLG"
static const uint8_t  LOG_VERSION = 4;               // 2、3 の記録も読める (devlog.h)
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

// 作りかけのフレーム
static uint8_t  s_frame[FRAME_MAX];
static bool     s_open   = false;
static uint8_t  s_n      = 0;
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
    s_stop.stopRelMs   = s_lastRel;
    s_stop.stopPhoneMs = phoneMs > 0 ? phoneMs : -1;
    flashPrepare();
    esp_partition_write(s_part, STOP_OFFSET, &s_stop, sizeof(s_stop));
}

static void closeFrame()
{
    if (!s_open) return;
    s_frame[2] = s_n;
    writeFrame(s_frame, FRAME_HDR + s_n * 3, s_n + 1);
    s_open = false;
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
    const int32_t t100 = (int32_t)((s_accTemp >= 0 ? s_accTemp + n / 2 : s_accTemp - n / 2) / n);
    uint8_t f[AVG_FRAME];
    f[0] = TAG_AVG;
    f[1] = s_header.session;
    put32(f + 2, (uint32_t)(s_accRel / (uint64_t)n));
    put24(f + 6, (uint32_t)((s_accRaw + n / 2) / n));
    put16(f + 9, (uint16_t)(int16_t)t100);
    put24(f + 11, (uint32_t)hpf & 0xFFFFFF);
    writeFrame(f, AVG_FRAME, 1);
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
    if (s_end + BATT_FRAME + FRAME_MAX + BOOT_FRAME > s_part->size) return;   // いっぱいになる直前
    closeFrame();
    uint8_t b[BATT_FRAME];
    b[0] = TAG_BATT;
    b[1] = s_header.session;
    put16(b + 2, mv);
    writeFrame(b, BATT_FRAME, 0);
    s_battMv = mv;
}

void devlogPush(int32_t raw, int32_t shift, float tempC, uint32_t batteryMv, int32_t hpf)
{
    if (!s_recording) return;
    writeBattery(batteryMv);
    uint32_t rel = nowRel();
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

    if (s_open) {
        const uint32_t dt = rel - s_lastRel;
        const int32_t  d  = raw - s_lastRaw;
        if (s_n < MAX_DELTAS && dt <= 255 && d >= -32768 && d <= 32767 && shift == s_shift) {
            uint8_t *p = s_frame + FRAME_HDR + s_n * 3;
            p[0] = (uint8_t)dt;
            put16(p + 1, (uint16_t)(int16_t)d);
            s_n++;
            s_lastRel = rel;
            s_lastRaw = raw;
            if (s_n >= MAX_DELTAS) closeFrame();
            return;
        }
        closeFrame();
    }

    // 新しいフレーム。入りきらなければ、いっぱいで止める
    if (s_end + FRAME_MAX + BOOT_FRAME > s_part->size) {
        stopRecording(0);
        s_full = true;
        Serial.println(F("# log: full"));
        return;
    }
    s_frame[0] = TAG_FRAME;
    s_frame[1] = s_header.session;
    s_frame[2] = 0;
    s_frame[3] = (uint8_t)shift;
    put32(s_frame + 4, rel);
    put24(s_frame + 8, (uint32_t)raw);
    put16(s_frame + 11, (uint16_t)(int16_t)lroundf(tempC * 100.0f));
    s_open    = true;
    s_n       = 0;
    s_shift   = shift;
    s_lastRel = rel;
    s_lastRaw = raw;
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
    uint8_t  h[8];
    while (off + BOOT_FRAME <= s_part->size) {
        esp_partition_read(s_part, off, h, sizeof(h));
        if (h[1] != s_header.session) break;
        if (h[0] == TAG_FRAME) {
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
    s_full = (s_stop.stopped == 0) && (s_end + FRAME_MAX + BOOT_FRAME > s_part->size);

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
    if (devlogIntervalMs() > 0) return devlogCount() + rest / AVG_FRAME;   // 平均 1 つで 14 バイト
    return devlogCount() + rest * 30 / 100;          // 30 サンプルで約 100 バイト
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
    uint8_t   frame[FRAME_MAX];
    uint8_t   n = 0, idx = 0;          // 読んでいるフレームの差分の数と、次のサンプル
    bool      inFrame = false;
    uint32_t  rel = 0;
    int32_t   raw = 0, shift = 8;
    int16_t   temp100 = 0;
    bool      avg = false;                 // 平均したサンプル (TAG_AVG)。変化量は avgHpf
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
            st->n       = 0;
            st->idx     = 0;
            st->inFrame = true;
            continue;
        }
        if (h[0] != TAG_FRAME) return false;
        st->avg = false;
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
    if (st->idx > 0) {
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

void devlogHandleCsv(AsyncWebServerRequest *req)
{
    if (!s_part || !s_valid) {
        req->send(404, "text/plain; charset=utf-8", "記録がありません");
        return;
    }
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

bool devlogDownloading()
{
    // 送り終わっていない応答が、最近まで進んでいた
    return s_dlTotal > 0 && s_dlSent < s_dlTotal && (millis() - s_dlLastMs) < 60000;
}

void devlogDownloadProgress(uint32_t &sent, uint32_t &total)
{
    sent  = s_dlSent;
    total = s_dlTotal;
}
