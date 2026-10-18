#include "devlog.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_random.h>
#include <sys/time.h>
#include <time.h>
#include <memory>

#include "settings.h"
#include "display.h"
#include "boot.h"
#include "high_pass_filter.h"

static const uint32_t SECTOR      = 4096;
static const uint32_t DATA_START  = SECTOR;          // 先頭の 1 セクタがヘッダ
static const uint32_t STOP_OFFSET = 64;              // 停止の情報の位置
static const uint8_t  TAG_FRAME   = 0xA5;
static const uint8_t  TAG_BOOT    = 0xB0;
static const uint32_t FRAME_HDR   = 13;
static const uint8_t  MAX_DELTAS  = 29;              // 1 フレーム = 30 サンプル (約 1 秒)
static const uint32_t FRAME_MAX   = FRAME_HDR + MAX_DELTAS * 3;
static const uint32_t BOOT_FRAME  = 6;
static const uint32_t RTC_MAGIC   = 0x504D4C47;      // "PMLG"

struct __attribute__((packed)) LogHeader {
    char     magic[4];
    uint8_t  version;           // 2
    uint8_t  session;
    int16_t  tzMin;
    int64_t  startEpochMs;      // 記録を始めたときの時刻 (スマホの時計)
    int64_t  hpfPrevX;          // 記録を始めたときの HPF の状態
    int64_t  hpfY;
    uint8_t  hpfInit;
    uint8_t  mode;
    uint8_t  reserved[30];
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

// 記録中であることを、再起動をまたいで覚えておく (電源が切れると消える)
static RTC_NOINIT_ATTR uint32_t s_rtcMagic;

// ページからの操作 (Web サーバのタスクで受け取り、loop() で行う)
enum Cmd : uint8_t { CMD_NONE, CMD_START, CMD_STOP, CMD_CLEAR };
static volatile Cmd     s_cmd = CMD_NONE;
static volatile int64_t s_cmdEpoch = 0;
static volatile int32_t s_cmdTz = 0;
static volatile bool    s_cmdWifiOff = false;
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

// ---------------------------------------------------------------- record

static void stopRecording(int64_t phoneMs)
{
    if (!s_recording) return;
    closeFrame();
    writeStop(phoneMs);
    s_recording = false;
    s_rtcMagic  = 0;
    Serial.printf("# log: recording stopped (%lu samples%s)\n", (unsigned long)s_count,
                  phoneMs > 0 ? ", time corrected" : "");
}

static void startRecording(int64_t epochMs, int32_t tzMin)
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
    s_header.version      = 2;
    s_header.session      = session;
    s_header.tzMin        = (int16_t)tzMin;
    s_header.startEpochMs = epochMs;
    const HighPassFilter::State st = s_hpf ? s_hpf->getState() : HighPassFilter::State{ 0, 0, false };
    s_header.hpfPrevX = st.prevX;
    s_header.hpfY     = st.y;
    s_header.hpfInit  = st.init ? 1 : 0;
    s_header.mode     = (uint8_t)g_set.mode;

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
    s_full      = false;
    s_recording = true;
    s_rtcMagic  = RTC_MAGIC ^ session;
    Serial.printf("# log: recording started (session %u)\n", session);
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

void devlogPush(int32_t raw, int32_t shift, float tempC)
{
    if (!s_recording) return;
    uint32_t rel = nowRel();
    if (rel < s_lastRel) rel = s_lastRel;

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
    s_valid = memcmp(s_header.magic, "PMLG", 4) == 0 && s_header.version == 2;
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
            startRecording(s_cmdEpoch, s_cmdTz);
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

void devlogRequestStart(int64_t epochMs, int32_t tzMin, bool wifiOff)
{
    Serial.printf("# log: start requested%s\n", wifiOff ? " (Wi-Fi off)" : "");
    s_cmdEpoch   = epochMs;
    s_cmdTz      = tzMin;
    s_cmdWifiOff = wifiOff;
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
    return devlogCount() + rest * 30 / 100;          // 30 サンプルで約 100 バイト
}

// ---------------------------------------------------------------- CSV
//
// 行の長さをそろえて (固定長)、ファイルの大きさを、作り始める前に決める。
// 大きさ (Content-Length) が分かると、スマホが保存の進み具合を表示できる。
// また、ESP32-C3 には浮動小数点の演算器がないので、数値は整数だけで文字にする (速い)。
//   time (23) , unix_ms (13) , elapsed_ms (8) , pressure_hPa (9) , raw (8) , hpf (8) , temperature_C (6) \r\n
//   例: 2026-10-18 12:34:56.789,1792310863620,00012345,1024.3348,06555743,-0000123,+27.34
static const char  CSV_HEADER[] =
    "\xEF\xBB\xBF" "time,unix_ms,elapsed_ms,pressure_hPa,raw,hpf,temperature_C\r\n";
static const size_t CSV_HEADER_LEN = sizeof(CSV_HEADER) - 1;
static const size_t CSV_LINE_LEN   = 83;

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
    char      line[CSV_LINE_LEN + 8];
    size_t    lineLen = 0, linePos = 0;
    bool      headerDone = false;
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
        if (h[0] != TAG_FRAME) return false;
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

// 次の 1 行を st->line に作る (必ず CSV_LINE_LEN 文字)。終わりなら false
static bool makeLine(CsvState *st)
{
    if (st->linesDone >= st->lines) return false;
    if (!nextSample(st)) {
        // 数えた行数より、読めたサンプルが少ない (起きないはず)。大きさを守るため、空の行で埋める
        memset(st->line, ' ', CSV_LINE_LEN - 2);
        st->line[CSV_LINE_LEN - 2] = '\r';
        st->line[CSV_LINE_LEN - 1] = '\n';
        st->lineLen = CSV_LINE_LEN;
        st->linePos = 0;
        st->linesDone++;
        return true;
    }

    // 変化量: 基板と同じ HPF で、気圧から計算し直す
    const int32_t hpf = st->hpf.update(st->raw, st->shift);

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

    int len = snprintf(st->line, sizeof(st->line),
        "%s.%03d,%013lld,%08lu,%04ld.%04ld,%08ld,%c%07ld,%c%02d.%02d\r\n",
        st->dateStr, (int)(localMs % 1000), (long long)unixMs, (unsigned long)relC,
        (long)(p10k / 10000) % 10000, (long)(p10k % 10000), (long)st->raw,
        hpfC < 0 ? '-' : '+', (long)(hpfC < 0 ? -hpfC : hpfC),
        t100 < 0 ? '-' : '+', (int)((t100 < 0 ? -t100 : t100) / 100), (int)((t100 < 0 ? -t100 : t100) % 100));
    if (len != (int)CSV_LINE_LEN) {
        // 想定外の桁数 (起きないはず)。大きさを守るため、行の長さをそろえる
        if (len > (int)CSV_LINE_LEN) len = CSV_LINE_LEN;
        for (int i = len - 2; i < (int)CSV_LINE_LEN - 2; i++) st->line[i] = ' ';
        st->line[CSV_LINE_LEN - 2] = '\r';
        st->line[CSV_LINE_LEN - 1] = '\n';
    }
    st->lineLen = CSV_LINE_LEN;
    st->linePos = 0;
    st->linesDone++;
    return true;
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

    const size_t total = CSV_HEADER_LEN + (size_t)st->lines * CSV_LINE_LEN;
    s_dlSent   = 0;
    s_dlTotal  = total;
    s_dlLastMs = millis();
    Serial.printf("# log: CSV download started (%lu lines, %lu bytes)\n",
                  (unsigned long)st->lines, (unsigned long)total);

    // ファイル名は、ページが付ける (英数字と - _ . だけを使う)
    String name = "pressure-monitor.csv";
    if (req->hasParam("name")) {
        const String n = req->getParam("name")->value();
        String clean;
        for (size_t i = 0; i < n.length() && clean.length() < 64; i++) {
            const char c = n[i];
            if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') clean += c;
        }
        if (clean.length() > 0) name = clean;
    }

    AsyncWebServerResponse *res = req->beginResponse("text/csv; charset=utf-8", total,
        [st, total](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
            size_t n = 0;
            if (!st->headerDone) {                 // 先頭: BOM と見出し
                st->headerDone = true;
                memcpy(st->line, CSV_HEADER, CSV_HEADER_LEN);
                st->lineLen = CSV_HEADER_LEN;
                st->linePos = 0;
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
    res->addHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
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
