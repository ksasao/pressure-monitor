#include "sound.h"

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "config.h"
#include "settings.h"

static const uint8_t BUZZER_ADDR    = 0x1E;     // 7 ビット (ライブラリの表記では 0x3C)
// 音の並びの中で、これより高い音は、1 オクターブ下げる (A7、約 3.5kHz)。
// 音の高さの設定 (PITCH_SHIFT) は、この後でずらす (ずらした量が、そのまま聞こえるように)
static const int     SOUND_NOTE_MAX = 105;

// ---------------------------------------------------------------- コード進行
// コードの音 (C を 0 とした半音の位置)。根音・3 度・5 度・7 度・9 度 (またはテンション) の順。
// 豪華さ (下の Level) に応じて、前から 3〜5 個を使う
struct Chord { int8_t pc[5]; };

// 明るい (気圧が上がる、+): C メジャー。I - V - vi - iii - IV - I - ii - V
static const Chord PROG_BRIGHT[8] = {
    {{ 0, 4, 7, 11, 2 }},       // Cmaj9
    {{ 7, 11, 2, 5, 9 }},       // G9
    {{ 9, 0, 4, 7, 11 }},       // Am9
    {{ 4, 7, 11, 2, 9 }},       // Em7 (add11)
    {{ 5, 9, 0, 4, 7 }},        // Fmaj9
    {{ 0, 4, 7, 11, 2 }},       // Cmaj9
    {{ 2, 5, 9, 0, 4 }},        // Dm9
    {{ 7, 11, 2, 5, 9 }},       // G9
};
// 暗い (気圧が下がる、-): C マイナー。i - VI - III - VII - iv - i - ii(-5) - V
static const Chord PROG_DARK[8] = {
    {{ 0, 3, 7, 10, 2 }},       // Cm9
    {{ 8, 0, 3, 7, 10 }},       // Abmaj9
    {{ 3, 7, 10, 2, 5 }},       // Ebmaj9
    {{ 10, 2, 5, 8, 0 }},       // Bb9
    {{ 5, 8, 0, 3, 7 }},        // Fm9
    {{ 0, 3, 7, 10, 2 }},       // Cm9
    {{ 2, 5, 8, 0, 3 }},        // Dm7(-5)
    {{ 7, 11, 2, 5, 8 }},       // G7(-9)。次の Cm へ戻る感じを出す
};
static const int PROG_LEN = 8;

// ---------------------------------------------------------------- 豪華さの段階
// 変化の大きさと、続いている時間から決める (0〜4)。段階が上がるほど、使うコードの音が増え、
// 音の数が多く、速く、広い音域になる。idx は、コードの音を低い方から並べたときの番号
struct Level {
    uint8_t tones;          // 使うコードの音の数 (3 = 三和音、4 = 7 度、5 = 9 度まで)
    uint8_t count;          // 1 小節の音の数
    uint8_t stepSparkle;    // 音の間隔 [ティック] (キラキラ)
    uint8_t stepFresh;      // 音の間隔 [ティック] (さわやか)
    uint8_t idx[2][12];     // 音の並び (2 種類を、小節ごとに使い分ける)
};
static const Level LEVELS[5] = {
    // 0: ささやか。三和音の 3 音
    { 3, 3,  4, 6, {{ 0, 1, 2 },
                    { 1, 2, 3 }} },
    // 1: 三和音の上がり下がり
    { 3, 4,  3, 5, {{ 0, 1, 2, 1 },
                    { 0, 2, 1, 3 }} },
    // 2: 7 度を加えた 6 音
    { 4, 6,  2, 4, {{ 0, 1, 2, 3, 2, 1 },
                    { 0, 2, 1, 3, 2, 4 }} },
    // 3: 7 度まで、1 オクターブ余りを 8 音で
    { 4, 8,  2, 3, {{ 0, 1, 2, 3, 4, 3, 2, 1 },
                    { 0, 2, 4, 1, 3, 5, 4, 2 }} },
    // 4: 9 度まで、2 オクターブ近くを 12 音で速く。最後に高い音を飾る (下の startBar)
    { 5, 12, 1, 2, {{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5 },
                    { 0, 2, 4, 6, 8, 9, 7, 5, 3, 5, 7, 9 }} },
};

// 鳴り始めるたびに、基準の高さを少しずつずらす [半音]。弱い変化が何度も続くときに、
// 同じ音ばかりにならないようにする (コードの音に合わせるので、外れた音にはならない)
static const int8_t WANDER[8] = { 0, 3, 5, 2, 7, 4, -2, 9 };

// 続いている時間による段階の境目 [ms]。0〜2 秒、2〜6 秒、6〜15 秒、15 秒〜
static const uint32_t PHASE_MS[3] = { 2000, 6000, 15000 };

// ---------------------------------------------------------------- 音の並び
struct Note {
    uint16_t freq;      // [Hz]
    uint16_t lenMs;     // 鳴らす長さ [ms]
    uint8_t  ticks;     // 次の音まで [ティック]
};

static bool     s_found = false;
static Note     s_seq[16];
static uint8_t  s_len = 0, s_pos = 0, s_wait = 0;
static bool     s_previewing = false;   // 試しに鳴らしている (不感帯でも止めない)
static volatile bool s_preview = false;

// 音楽の状態
static bool     s_haveKey = false;      // 調とコードの位置が有効か (長い無音のあとは、主和音から)
static bool     s_bright  = true;       // 今の調。true = メジャー (+)、false = マイナー (-)
static uint8_t  s_chord   = 0;          // 次の小節のコード (進行の中の位置)
static bool     s_active  = false;      // 不感帯の外にいる
static uint32_t s_onTicks = 0;          // 今の変化が続いているティック数
static uint32_t s_offTicks = 0;         // 無音が続いているティック数
static uint8_t  s_motif   = 0;          // 鳴り始めた回数 (基準の高さと、音の並びを変える)
static uint16_t s_bar     = 0;          // 今の変化の中で、何小節目か
static int32_t  s_barPeak = 0;          // 今の小節の間の、変化量の絶対値の最大
static uint32_t s_lastBurstMs = 0;
static bool     s_burstDone = false;    // 一度でも駆け上がりを鳴らしたか
static int32_t  s_hist[SOUND_BURST_TICKS];   // 変化量の絶対値の履歴 (瞬間的な変化の判定)
static uint8_t  s_histPos = 0;

static bool sendTone(uint32_t freq, uint32_t lenMs)
{
    uint8_t b[8];
    memcpy(b, &freq, 4);                // ESP32-C3 はリトルエンディアン (Modulino と同じ)
    memcpy(b + 4, &lenMs, 4);
    Wire.beginTransmission(BUZZER_ADDR);
    Wire.write(b, sizeof(b));
    return Wire.endTransmission() == 0;
}

static uint16_t midiToHz(int m)
{
    return (uint16_t)lroundf(440.0f * powf(2.0f, (m - 69) / 12.0f));
}

// 音の高さ (settings の soundPitch): 全ての音をずらす [半音]。圧電ブザーは約 4kHz で最も大きく、
// 低い音ほど小さく鳴るので、音量も変わる (本当の音量の調整は、できないため)
static const int PITCH_SHIFT[SOUND_PITCH_COUNT] = { 12, 0, -12, -24 };   // 高い、標準、低い、とても低い

static void add(int midi, uint16_t lenMs, uint8_t ticks)
{
    if (s_len >= sizeof(s_seq) / sizeof(s_seq[0])) return;
    int v = g_set.soundPitch;
    if (v < 0 || v >= SOUND_PITCH_COUNT) v = SOUND_PITCH_NORMAL;
    // 高すぎる音を音域の中に戻してから、音の高さの設定でずらす
    while (midi > SOUND_NOTE_MAX) midi -= 12;
    s_seq[s_len++] = { midiToHz(midi + PITCH_SHIFT[v]), lenMs, ticks };
}

static void clearSeq()
{
    s_len = 0;
    s_pos = 0;
    s_wait = 0;
}

// 基準の音の高さ (MIDI の音番号)。変化量の絶対値 a (0〜1000) に比例させる
static int baseNote(int32_t a)
{
    if (a > 1000) a = 1000;
    return SOUND_BASE_LOW + (int)((SOUND_BASE_HIGH - SOUND_BASE_LOW) * a / 1000);
}

// コードの音を、base 以上で低い順に count 個並べる (n = 使う音の数)。
// base に最も近いコードの音から始まるので、小節ごとに基準の高さが変わっても、つながりが自然になる
static int chordTones(const Chord &c, int n, int base, int *out, int count)
{
    int k = 0;
    for (int m = base; m < base + 48 && k < count; m++) {
        const int pc = m % 12;
        for (int i = 0; i < n; i++) {
            if (c.pc[i] == pc) { out[k++] = m; break; }
        }
    }
    return k;
}

static const Chord &currentChord()
{
    return s_bright ? PROG_BRIGHT[s_chord] : PROG_DARK[s_chord];
}

// 続いている時間の段階 (0〜3)
static int phaseOf(uint32_t onMs)
{
    int d = 0;
    while (d < 3 && onMs >= PHASE_MS[d]) d++;
    return d;
}

// 1 小節分のアルペジオ。a は、この小節の基準にする変化量の絶対値
static void startBar(int32_t a)
{
    clearSeq();

    // 豪華さ: 変化の大きさ (0〜3) と、続いている時間 (0〜3) から決める
    const int m = (a < 200) ? 0 : (a < 450) ? 1 : (a < 700) ? 2 : 3;
    const int d = phaseOf(s_onTicks * TICK_INTERVAL_MS);
    int level = (m * 2 + d + 1) / 2;
    if (level > 4) level = 4;
    const Level &L = LEVELS[level];

    // 基準の高さ: 変化の大きさ + 鳴り始めごとのずらし + 長く続くほど少しずつ上げる (盛り上げる)
    const int base = baseNote(a) + WANDER[s_motif % 8] + d * 2;

    int t[16];
    const int k = chordTones(currentChord(), L.tones, base, t, 16);

    // 音の並び: 2 種類を使い分け、さらに 1 小節おきに逆順にする (同じ形が続かないように)
    const uint8_t *idx = L.idx[(s_bar + s_motif) % 2];
    const bool reverse = ((s_bar + s_motif / 2) % 2) == 1 && level < 4;
    const bool sparkle = (g_set.sound == SOUND_SPARKLE);
    const uint8_t step = sparkle ? L.stepSparkle : L.stepFresh;
    const uint16_t len = (uint16_t)(step * TICK_INTERVAL_MS - (sparkle ? 8 : 20));

    for (int i = 0; i < L.count; i++) {
        int j = idx[reverse ? (L.count - 1 - i) : i];
        if (j >= k) j = k - 1;
        add(t[j], len, step);
    }
    // いちばん豪華な段階: 最後に、1 オクターブ上のコードの音を、長めに鳴らして飾る
    if (level == 4) add(t[0] + 24, (uint16_t)(3 * step * TICK_INTERVAL_MS), (uint8_t)(3 * step));

    s_bar++;
    s_chord = (uint8_t)((s_chord + 1) % PROG_LEN);
}

// 瞬間的な変化: 今のコードの音で、速く駆け上がる。変化が大きいほど、音を多く、高くする
static void startBurst(int32_t a)
{
    clearSeq();
    const int tones = (a < 450) ? 3 : (a < 700) ? 4 : 5;
    const int count = (a < 450) ? 6 : (a < 700) ? 8 : 10;
    int t[12];
    const int k = chordTones(currentChord(), tones, baseNote(a) - 7, t, count);
    const bool sparkle = (g_set.sound == SOUND_SPARKLE);
    for (int i = 0; i < k; i++) {
        const bool last = (i == k - 1);
        if (sparkle) add(t[i], last ? 160 : 30, last ? 6 : 1);    // 約 33ms ごと
        else         add(t[i], last ? 240 : 60, last ? 8 : 2);    // 約 66ms ごと
    }
}

// ---------------------------------------------------------------- public

void soundBegin()
{
    Wire.beginTransmission(BUZZER_ADDR);
    s_found = (Wire.endTransmission() == 0);
    Serial.printf("# sound: Modulino Buzzer %s\n", s_found ? "found" : "not found");
}

bool soundAvailable()
{
    return s_found;
}

void soundRequestPreview()
{
    s_preview = true;
}

void soundTick(uint32_t nowMs, int32_t level)
{
    if (!s_found) return;

    // 鳴らしている途中の音の並び: 次の音を送る
    if (s_wait > 0) {
        s_wait--;
    } else if (s_pos < s_len) {
        const Note &n = s_seq[s_pos++];
        sendTone(n.freq, n.lenMs);
        s_wait = n.ticks > 0 ? n.ticks - 1 : 0;
    }
    const bool playing = (s_pos < s_len) || (s_wait > 0);
    if (!playing) s_previewing = false;

    // 変化量の絶対値の履歴 (SOUND_BURST_TICKS 前の値と比べる)
    const int32_t a    = level < 0 ? -level : level;
    const int32_t past = s_hist[s_histPos];
    s_hist[s_histPos] = a;
    s_histPos = (uint8_t)((s_histPos + 1) % SOUND_BURST_TICKS);

    // 設定ページで音色を変えた: 試しに鳴らす (明るい調の、いちばん豪華な小節)
    if (s_preview) {
        s_preview = false;
        if (g_set.sound != SOUND_OFF) {
            s_bright  = true;
            s_chord   = 0;
            s_haveKey = true;
            s_onTicks = PHASE_MS[2] / TICK_INTERVAL_MS;
            startBar(800);
            s_onTicks = 0;
            s_previewing = true;
        }
        return;
    }
    if (s_previewing) return;

    // 不感帯 (または音を鳴らさない設定): 無音にする。鳴っている音も、すぐに止める。
    // 短い無音では、コード進行の位置を覚えておき、次は続きから鳴らす (同じ音が続かないように)。
    // 無音が SOUND_RESET_MS 続いたら、次は主和音から
    if (a == 0 || g_set.sound == SOUND_OFF || nowMs < SOUND_WARMUP_MS) {
        if (playing) sendTone(0, 0);        // 0 Hz = 止める (ライブラリの noTone() と同じ)
        clearSeq();
        s_active = false;
        if (++s_offTicks * TICK_INTERVAL_MS >= SOUND_RESET_MS) s_haveKey = false;
        return;
    }

    // 鳴り始め: 基準の高さと音の並びを変える
    if (!s_active) {
        s_active   = true;
        s_onTicks  = 0;
        s_bar      = 0;
        s_barPeak  = 0;
        s_motif++;
    }
    s_offTicks = 0;
    s_onTicks++;
    if (a > s_barPeak) s_barPeak = a;

    // 調: + (気圧が上がる) は明るいメジャー、- は暗いマイナー (C メジャーと C マイナー)。
    // 入れ替わっても、コード進行の位置はそのまま (同じ位置のコードを、もう一方の調で鳴らす)
    if (!s_haveKey) {
        s_haveKey = true;
        s_chord   = 0;
    }
    s_bright = level > 0;

    // 瞬間的な変化: 今の小節を打ち切って、駆け上がりを鳴らす
    if (a - past >= SOUND_BURST_DELTA &&
        (!s_burstDone || nowMs - s_lastBurstMs >= SOUND_BURST_GAP_MS)) {
        startBurst(a);
        s_lastBurstMs = nowMs;
        s_burstDone = true;
        return;
    }

    // 小節が終わったら、次のコードへ進む。豪華さは、前の小節の間の最大の変化量で決める
    // (小節の途中で一瞬小さくなっても、急に寂しくならないように)
    if (!playing) {
        startBar(s_barPeak > a ? s_barPeak : a);
        s_barPeak = a;
    }
}
