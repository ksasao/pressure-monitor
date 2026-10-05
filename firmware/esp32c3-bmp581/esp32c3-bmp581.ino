/*
 * PressureMonitor - bring-up sketch (FastLED 版 / 30Hz インタラクティブ)
 *
 *   MCU    : ESP32-C3 (native USB CDC)
 *   Sensor : BMP581  (I2C, SDO=GND -> 0x46)
 *   LED    : WS2812B-V6 x5 (GPIO10 -> R8 330R -> LED1.DIN)
 *
 * 必要ライブラリ (Arduino IDE: ライブラリマネージャ)
 *   - "SparkFun BMP581 Arduino Library"
 *   - "FastLED"   3.7.0 以降を推奨
 *
 * ボード設定 (Arduino IDE)
 *   Board            : ESP32C3 Dev Module
 *   USB CDC On Boot  : Enabled      <-- Disabled だと Serial が出ません
 *
 * --- 気圧変化の表示ロジック ---
 *   1. 気圧の値を整数のまま 1 次ハイパスフィルタ (HPF) に通す
 *      -> ゆっくりしたドリフト (自己発熱など) は消え、素早い変化だけが残る
 *   2. HPF 出力 (delta) の絶対値が DELTA_LIMIT 未満なら黒 (不感帯)
 *   3. DELTA_RANGE を上限に、4 点のパレットを線形補間して色にする
 *        気圧上昇: 黒青 -> 青 -> 水色 -> 白
 *        気圧下降: 黒赤 -> 橙 -> 黄橙 -> 白
 *   4. 毎フレーム、色を 1 つずつ後ろの LED に送る (LED1 が最新)
 *
 * 気圧の値について: BMP581 は温度補正を内蔵していて、気圧レジスタは
 * 補正済みの Pa x 64 (1 LSB = 1/64 Pa) で出てきます。この値を
 * そのまま HPF の入力にします。
 *
 * --- 設計方針: 遅延をゼロにする ---
 * センサ読み取り / ログ / 描画を 1 本の 30Hz ティックにまとめています。
 * 平滑化フィルタは HPF だけで、LPF や補間はありません。
 */

#include <Wire.h>
#include <math.h>
#include <SparkFun_BMP581_Arduino_Library.h>

#define FASTLED_INTERNAL          // 「FastLED version ...」の警告を抑制
#include <FastLED.h>

// ---------------------------------------------------------------- pin map
// 回路図 P1 より
#define PIN_NEO 10                // U2 IO10 -> R8 -> LED1 DIN
                                  // FastLED はピンをテンプレート引数に取るため
                                  // コンパイル時定数である必要があります

static const uint8_t PIN_SDA  = 4;    // U2 IO4  -> U1 SDI
static const uint8_t PIN_SCL  = 5;    // U2 IO5  -> U1 SCK
static const uint8_t PIN_BOOT = 9;    // U2 IO9  (タクトスイッチ, active low)

static const uint8_t NUM_LEDS = 5;

// BMP581 の I2C アドレス: SDO=GND -> 0x46 / SDO=VDDIO -> 0x47
static const uint8_t ADDR_SDO_LOW  = 0x46;
static const uint8_t ADDR_SDO_HIGH = 0x47;

// ------------------------------------------------------------------ rate
// センサ読み取り / ログ / 描画を全部この周期で回します
static const uint32_t TICK_INTERVAL_MS = 33;        // ≒ 30 Hz

// ログの間引き。1 = 毎ティック出力 (30 行/秒)
static const uint8_t LOG_EVERY_N = 1;

// 1 秒ごとに「どこが遅いか」を '#' 付きの 1 行で出力する
static const bool ENABLE_PERF_LOG = true;

// ----------------------------------------------------------------- power
// CPU クロック [MHz]。1 ティック (33ms) の処理は数 ms なので 40 で足ります。
// 40MHz で 1 ティックの処理は約 4.5ms (33ms の 14%)、LED の表示も
// 変わらないことを確認済みです。
// 80MHz 未満では APB クロックも CPU に連動して下がります。
// LED や USB がおかしくなったら 80 (または 160) に戻してください
static const uint32_t CPU_MHZ = 40;

// --------------------------------------------------------------- battery
// 電池電圧 (AA(+)x2) は、R9/R10 (各 1MΩ) で 1/2 に分圧して IO1 に入っています。
// 1 分に 1 回、'#' で始まる 1 行でシリアルに出力します
static const uint8_t  PIN_VBAT            = 1;       // U2 IO1 (ADC1_CH1)
static const uint32_t BATTERY_INTERVAL_MS = 60000;   // 1 分
static const uint32_t BATTERY_DIVIDER     = 2;       // R9 = R10 = 1MΩ
static const uint8_t  BATTERY_SAMPLES     = 16;      // 平均する回数
static const uint32_t BATTERY_PRESENT_MV  = 1000;    // これ未満は電池なしとみなす

// ----------------------------------------------------- high-pass filter
// y[n] = y[n-1] + (x[n] - x[n-1]) - y[n-1] / 2^FILTER_STRENGTH_SHIFT
// 時定数は「サンプル数」で決まり、約 2^SHIFT サンプルです。
//   30Hz で SHIFT=8 (256 サンプル) なら 8.5 秒
// 大きいほどゆっくりした変化まで拾い、戻りが遅くなります
static const int32_t FILTER_STRENGTH_SHIFT   = 8;

// 内部計算の精度を上げるための追加シフト
static const int32_t INTERNAL_PRECISION_SHIFT = 8;

// -------------------------------------------------- display (mode 0)
// 単位は 1/64 Pa。1 Pa ≒ 8.5 cm の高さ変化です。
//
//   DELTA_LIMIT 50   ≒ 0.78 Pa ≒ 6.6 cm 未満は消灯
//   DELTA_RANGE 400  ≒ 6.25 Pa ≒ 53 cm で白に達する (細かい動き向け)
//
// 既定値は細かい動き向けの 400 です。
// 広い範囲 (エレベータ等) を見たいときは DELTA_RANGE = 15000 (≒ 20 m)。
//
// 感度は、ログの hpf 列を見て調整してください。
// 静止時の hpf の振れ幅より DELTA_LIMIT が小さいと、暗い色がちらつきます
static const int32_t DELTA_LIMIT = 50;
static const int32_t DELTA_RANGE = 400;

// 色を後ろへ送る間隔 [ティック]。1 = 毎フレーム
// 大きくすると LED2..5 に長い時間の履歴が流れます (LED1 は常にライブ値)
static const uint8_t TRAIL_STEP_TICKS = 1;

// パレットの点数。(DELTA_RANGE - DELTA_LIMIT) をこの数 - 1 で割って区間にします
static const int32_t PALETTE_STEPS = 4;
static_assert(DELTA_RANGE - DELTA_LIMIT >= PALETTE_STEPS - 1,
              "DELTA_RANGE must be larger than DELTA_LIMIT");

// 上昇 (青系) / 下降 (赤橙系)。r, g, b
static const uint8_t PAL_RISE[PALETTE_STEPS][3] = {
    {  0,   0,   1}, {  0,  48, 128}, { 64, 192, 255}, {255, 255, 255}
};
static const uint8_t PAL_FALL[PALETTE_STEPS][3] = {
    {  1,   0,   0}, {128,  48,   0}, {255, 192,  40}, {255, 255, 255}
};

// --------------------------------------------------- bar display (mode 1)
static const float   BAR_MIN_HPA   = 995.0f;
static const float   BAR_MAX_HPA   = 1025.0f;
static const uint8_t BAR_MAX_VALUE = 120;

// ---------------------------------------------------------------- tuning
static const uint8_t SELFTEST_VALUE = 120;
static const uint8_t ERROR_VALUE    = 120;

// 消費電力の上限。FastLED が自動で輝度を抑えてくれます。
// 白は 5 個全点灯で電流が最大になるので、電池駆動ではこの制限が効きます。
// FastLED の電流モデルは 5V の WS2812B 基準なので、3.3V 動作では
// 実際より多めに見積もられます (= 安全側に効きます)
static const uint8_t  PWR_VOLTS = 3;
static const uint32_t PWR_MILLIAMPS = 250;

// 標高計算に使う基準気圧 [hPa]
static const float SEA_LEVEL_HPA = 1013.25f;

// ---------------------------------------------------------------- globals
BMP581 bmp;
CRGB   leds[NUM_LEDS];

static uint8_t  g_bmpAddr  = 0;
static bool     g_bmpReady = false;

// HPF の状態
static int64_t  g_hpfPrevX = 0;
static int64_t  g_hpfY     = 0;
static bool     g_hpfInit  = false;

static float    g_lastHpa = 0.0f;     // バー表示用に最新値を保持

// LED1 が今まさに表示している色 (ライブ値)
static CRGB     g_head = CRGB::Black;

// LED2..LED5 用の履歴。LED i は i フレーム前の LED1 の色
static CRGB     g_trail[NUM_LEDS];
static uint8_t  g_shiftCount = 0;

static uint32_t g_lastTick = 0;
static uint32_t g_lastBatMs = 0;      // 最後に電池電圧を出力した時刻
static uint8_t  g_logCount = 0;

// 性能計測 (1 秒ごとに出力してリセット)
static uint32_t g_tI2cMax = 0, g_tLogMax = 0, g_tRenderMax = 0, g_periodMax = 0;
static uint32_t g_ticks = 0, g_logDropped = 0, g_i2cErrors = 0, g_perfT = 0;

// ログの送信状況 (リセットしない累計値)
static uint32_t g_dropTotal = 0;      // 入りきらずに捨てた行の累計
static uint32_t g_lastLogOkMs = 0;    // 最後に 1 行まるごと書き込めた時刻

// 表示モード: 0 = 気圧変化(HPF), 1 = 気圧バー, 2 = 消灯(省電力)
static uint8_t  g_mode = 0;

// ---------------------------------------------------------------- helpers

static void ledSweep(const CHSV &color, uint16_t stepMs)
{
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        leds[i] = color;
        FastLED.show();
        delay(stepMs);
    }
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
}

// 起動時の自己診断: 全 LED を順に R -> G -> B
// N 個目で止まる場合は、その手前の LED かデータ配線が原因です
static void ledSelfTest()
{
    ledSweep(CHSV(0,   255, SELFTEST_VALUE), 60);   // red
    ledSweep(CHSV(96,  255, SELFTEST_VALUE), 60);   // green
    ledSweep(CHSV(160, 255, SELFTEST_VALUE), 60);   // blue
}

// エラー表示: 全点灯で赤く明滅
static void ledError()
{
    static bool on = false;
    on = !on;
    fill_solid(leds, NUM_LEDS,
               on ? CHSV(0, 255, ERROR_VALUE) : CHSV(0, 0, 0));
    FastLED.show();
}

static void i2cScan()
{
    Serial.println(F("--- I2C scan ---"));
    uint8_t found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  found device at 0x%02X\n", addr);
            found++;
        }
    }
    if (found == 0) {
        Serial.println(F("  no device found."));
        Serial.println(F("  -> SDA/SCL(IO4/IO5), 3V3, プルアップ R5/R6, "
                         "U1 の実装/はんだを確認してください"));
    }
    Serial.println(F("----------------"));
}

// BMP581 を 0x46 -> 0x47 の順で探す
static bool bmpBegin()
{
    const uint8_t candidates[] = { ADDR_SDO_LOW, ADDR_SDO_HIGH };

    for (uint8_t i = 0; i < sizeof(candidates); i++) {
        if (bmp.beginI2C(candidates[i], Wire) == BMP5_OK) {
            g_bmpAddr = candidates[i];
            Serial.printf("BMP581 found at 0x%02X\n", g_bmpAddr);
            if (g_bmpAddr == ADDR_SDO_HIGH) {
                Serial.println(F("  [note] 回路図では SDO=GND なので 0x46 の想定です。"
                                 "SDO の接続を確認してください"));
            }
            return true;
        }
    }
    return false;
}

// 30Hz で読むための設定。
//
// 128X オーバーサンプリングは 1 回の変換に 160ms 前後かかるため、
// 30Hz (33ms) には間に合いません。応答性を取るぶん分解能を落とします。
// 温度は補正と表示にしか使わないので 1X まで削って、その時間を気圧に回します。
//
// [!] BMP5_ODR_50_HZ / BMP5_OVERSAMPLING_* のマクロ名はライブラリの
//     バージョンで異なることがあります。コンパイルが通らない場合は
//     bmp5_defs.h の定義を確認してください。
static bool bmpConfigure()
{
    int8_t err = bmp.setODRFrequency(BMP5_ODR_50_HZ);
    if (err != BMP5_OK) {
        Serial.printf("setODRFrequency failed (%d)\n", err);
        return false;
    }

    bmp5_osr_odr_press_config cfg = {0};
    err = bmp.getOSRMultipliers(&cfg);
    if (err != BMP5_OK) {
        Serial.printf("getOSRMultipliers failed (%d)\n", err);
        return false;
    }

    cfg.osr_t    = BMP5_OVERSAMPLING_1X;
    cfg.osr_p    = BMP5_OVERSAMPLING_16X;
    cfg.press_en = BMP5_ENABLE;

    err = bmp.setOSRMultipliers(&cfg);
    if (err != BMP5_OK) {
        Serial.printf("setOSRMultipliers failed (%d)\n", err);
        return false;
    }
    return true;
}

// 設定直後はデータレジスタにリセット値が残っており、そのまま読むと
// 1305 hPa / 127.5 degC のような値が出る。最初の変換が終わるまで捨てる
static void bmpWarmUp()
{
    delay(150);
    bmp5_sensor_data scratch = {0, 0};
    for (uint8_t i = 0; i < 5; i++) {
        bmp.getSensorData(&scratch);
        delay(30);
    }
}

// 明らかに異常な値を弾く。I2C が化けた場合にも効く
static bool isPlausible(const bmp5_sensor_data &d)
{
    float hPa = d.pressure / 100.0f;
    return (hPa > 300.0f && hPa < 1200.0f &&
            d.temperature > -40.0f && d.temperature < 85.0f);
}

static float toAltitude(float hPa)
{
    return 44330.0f * (1.0f - powf(hPa / SEA_LEVEL_HPA, 0.1903f));
}

// ------------------------------------------------------------ HPF / color

// 0 方向ではなく「ゼロから遠ざかる方向」に丸める (round half away from zero)
static inline int64_t roundShiftAway(int64_t v, int32_t shift)
{
    int64_t a = (v < 0) ? -v : v;
    int64_t r = (a + (1LL << (shift - 1))) >> shift;
    return (v < 0) ? -r : r;
}

// 1 次ハイパスフィルタ (固定小数点)。
// 初回は入力値で内部状態を初期化するので、起動時に過渡応答は出ません
static int32_t hpfUpdate(int32_t x)
{
    int64_t cx = (int64_t)x << INTERNAL_PRECISION_SHIFT;

    if (!g_hpfInit) {
        g_hpfPrevX = cx;
        g_hpfY     = 0;
        g_hpfInit  = true;
    }

    int64_t diff  = cx - g_hpfPrevX;
    int64_t decay = roundShiftAway(g_hpfY, FILTER_STRENGTH_SHIFT);

    g_hpfY     = g_hpfY + diff - decay;
    g_hpfPrevX = cx;

    return (int32_t)roundShiftAway(g_hpfY, INTERNAL_PRECISION_SHIFT);
}

// delta -> 色。delta > 0 (気圧上昇) は青系、delta < 0 (下降) は赤橙系
static CRGB deltaToColor(int32_t delta)
{
    int32_t a = (delta >= 0) ? delta : -delta;
    if (a < DELTA_LIMIT) return CRGB::Black;
    if (a > DELTA_RANGE) a = DELTA_RANGE;

    const int32_t width = (DELTA_RANGE - DELTA_LIMIT) / (PALETTE_STEPS - 1);
    int32_t index = (a - DELTA_LIMIT) / width;
    int32_t mid   = (a - DELTA_LIMIT) - index * width;

    // 上端ちょうど (a == DELTA_RANGE) は最後の点を使う。
    // (index + 1 がパレットの配列の範囲外にならないようにするため)
    if (index >= PALETTE_STEPS - 1) {
        index = PALETTE_STEPS - 2;
        mid   = width;
    }

    const uint8_t (*pal)[3] = (delta > 0) ? PAL_RISE : PAL_FALL;

    CRGB c;
    c.r = (uint8_t)((pal[index][0] * (width - mid) + pal[index + 1][0] * mid) / width);
    c.g = (uint8_t)((pal[index][1] * (width - mid) + pal[index + 1][1] * mid) / width);
    c.b = (uint8_t)((pal[index][2] * (width - mid) + pal[index + 1][2] * mid) / width);
    return c;
}

// 履歴を 1 段後ろにずらして、先頭に今の色を入れる
static void trailShift(const CRGB &head)
{
    for (uint8_t i = NUM_LEDS - 1; i >= 1; i--) {
        g_trail[i] = g_trail[i - 1];
    }
    g_trail[0] = head;
}

// 30Hz で呼ばれる。LED1 はライブ値、LED2..5 は履歴
static void renderTrail()
{
    leds[0] = g_head;
    for (uint8_t i = 1; i < NUM_LEDS; i++) leds[i] = g_trail[i];
    FastLED.show();
}

// 気圧の絶対値を 5 段階のバーで表示 (低圧=青 -> 高圧=赤)
// FastLED の hue は 0=赤 / 96=緑 / 160=青 の 8bit
static void showBar(float hPa)
{
    float t = (hPa - BAR_MIN_HPA) / (BAR_MAX_HPA - BAR_MIN_HPA);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float lit = t * NUM_LEDS;   // 何個分点灯するか (小数)

    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        float f = lit - (float)i;           // この LED の点灯率 0..1
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;

        uint8_t hue = 160 - (uint8_t)(i * (160 / (NUM_LEDS - 1)));  // 160 -> 0
        leds[i] = CHSV(hue, 255, (uint8_t)(f * BAR_MAX_VALUE));
    }
    FastLED.show();
}

// BOOT ボタンの立ち下がりで表示モードを切り替え
static void pollButton()
{
    static bool     prev     = true;
    static uint32_t lastEdge = 0;

    bool now = digitalRead(PIN_BOOT);
    if (prev && !now && (millis() - lastEdge) > 200) {
        lastEdge = millis();
        g_mode = (g_mode + 1) % 3;
        Serial.printf("# mode -> %u\n", g_mode);

        if (g_mode == 2) {
            fill_solid(leds, NUM_LEDS, CRGB::Black);
            FastLED.show();
        }
    }
    prev = now;
}

// センサを 1 回読んで HPF と LED1 の色を更新する
static void sampleOnce(uint32_t nowMs)
{
    bmp5_sensor_data data = {0, 0};
    uint32_t tA = micros();
    int8_t err = bmp.getSensorData(&data);
    uint32_t tI2c = micros() - tA;
    if (tI2c > g_tI2cMax) g_tI2cMax = tI2c;
    if (err != BMP5_OK) {
        g_i2cErrors++;
        Serial.printf("# getSensorData failed (%d)\n", err);
        return;
    }

    if (!isPlausible(data)) {
        Serial.printf("# discarded: %.2f hPa, %.2f degC\n",
                      data.pressure / 100.0f, data.temperature);
        return;
    }

    float hPa  = data.pressure / 100.0f;   // ライブラリは Pa で返す
    float degC = data.temperature;
    float alt  = toAltitude(hPa);

    g_lastHpa = hPa;

    // BMP581 の気圧レジスタは 1 LSB = 1/64 Pa。float は Pa x 64 を
    // 厳密に表せる (2 のべき乗倍) ので、整数に戻しても情報は落ちません
    int32_t raw = (int32_t)lroundf(data.pressure * 64.0f);

    // BMP581 の値は気圧上昇で増えるので、符号を反転せずそのまま使います
    // (delta > 0 が気圧上昇)
    int32_t hpf   = hpfUpdate(raw);
    int32_t delta = hpf;

    CRGB color = deltaToColor(delta);

    // 平滑化せずそのまま反映する。ここが応答性の要です
    g_head = color;

    if (++g_logCount >= LOG_EVERY_N) {
        g_logCount = 0;
        uint8_t lv = color.r;
        if (color.g > lv) lv = color.g;
        if (color.b > lv) lv = color.b;

        char buf[96];
        int len = snprintf(buf, sizeof(buf), "%lu,%.3f,%ld,%ld,%.2f,%.2f,%u\n",
                           (unsigned long)nowMs, hPa, (long)raw, (long)hpf,
                           degC, alt, lv);
        if (len > (int)sizeof(buf) - 1) len = sizeof(buf) - 1;   // 切り詰められた場合の保険

        // 空きを見てから書くのではなく、毎回必ず書き込みを試みる。
        // 事前に弾くと書き込みが一切行われなくなり、HWCDC が送信割り込みを
        // 再開する処理 (write() の中にある) が動かず、ログが止まったままに
        // なることがある。待ち時間は 0 なので、入りきらなければ捨てるだけ
        uint32_t tB = micros();
        size_t n = Serial.write((const uint8_t *)buf, len);
        uint32_t tLog = micros() - tB;
        if (tLog > g_tLogMax) g_tLogMax = tLog;

        if (n == (size_t)len) {
            g_lastLogOkMs = nowMs;
        } else {
            g_logDropped++;
            g_dropTotal++;
        }
    }
}

// ------------------------------------------------------------ battery

// 電池電圧 [mV] を返す。分圧 (1/2) を戻した値です。
// 分圧の出力抵抗が約 500kΩ と高いので、C12 (100nF) に頼って複数回読んで平均する。
// ADC の誤差は数 % あるので、気になる場合はテスターの実測と突き合わせてください
static uint32_t readBatteryMilliVolts()
{
    analogReadMilliVolts(PIN_VBAT);          // 1 回目は捨てる

    uint32_t sum = 0;
    for (uint8_t i = 0; i < BATTERY_SAMPLES; i++) {
        sum += analogReadMilliVolts(PIN_VBAT);
    }
    return (sum / BATTERY_SAMPLES) * BATTERY_DIVIDER;
}

// 例: "# battery 2.874 V"
//     電池が入っていない (USB 給電中など) ときは "(not installed)" が付く
static void reportBattery()
{
    uint32_t mV = readBatteryMilliVolts();
    Serial.printf("# battery %lu.%03lu V%s\n",
                  (unsigned long)(mV / 1000), (unsigned long)(mV % 1000),
                  (mV < BATTERY_PRESENT_MV) ? " (not installed)" : "");
}

// ---------------------------------------------------------------- setup

void setup()
{
    setCpuFrequencyMhz(CPU_MHZ);    // Serial / I2C / FastLED の初期化より前に
    Serial.begin(115200);
    // 送信バッファが埋まっても待たない (ログを落として LED を優先する)。
    // USB CDC On Boot = Enabled のとき Serial は HWCDC で、この関数があります
    Serial.setTxTimeoutMs(0);

    // USB CDC の接続待ち (繋がなくても先に進む)
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) delay(10);

    Serial.println();
    Serial.println(F("=== PressureMonitor bring-up (FastLED) ==="));

    pinMode(PIN_BOOT, INPUT_PULLUP);

    for (uint8_t i = 0; i < NUM_LEDS; i++) g_trail[i] = CRGB::Black;

    FastLED.addLeds<WS2812B, PIN_NEO, GRB>(leds, NUM_LEDS);
    FastLED.setMaxPowerInVoltsAndMilliamps(PWR_VOLTS, PWR_MILLIAMPS);
    FastLED.setBrightness(255);     // 素通し。明るさはパレットの値で決める
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
    ledSelfTest();

    Wire.begin(PIN_SDA, PIN_SCL, 400000);

    i2cScan();

    if (!bmpBegin()) {
        Serial.println(F("BMP581 not responding."));
        Serial.println(F("  確認項目: CSB が VDDIO にプルアップされているか "
                         "(GND だと SPI モードになり I2C から見えません)"));
        g_bmpReady = false;
        return;
    }

    g_bmpReady = bmpConfigure();
    if (!g_bmpReady) {
        Serial.println(F("BMP581 configuration failed."));
        return;
    }

    bmpWarmUp();

    Serial.printf("# tick %lu ms (%lu Hz) / HPF shift %ld"
                  " / limit %ld range %ld (1/64 Pa)\n",
                  (unsigned long)TICK_INTERVAL_MS,
                  1000UL / TICK_INTERVAL_MS,
                  (long)FILTER_STRENGTH_SHIFT,
                  (long)DELTA_LIMIT, (long)DELTA_RANGE);
    Serial.printf("# cpu %lu MHz\n", (unsigned long)getCpuFrequencyMhz());
    reportBattery();                // 起動時に 1 回。以降は 1 分ごと
    Serial.println(F("ready."));
    Serial.println(F("millis,pressure_hPa,raw,hpf,"
                     "temperature_C,altitude_m,led_value"));

    uint32_t now = millis();
    g_lastTick  = now;
    g_perfT     = now;
    g_lastBatMs = now;
}

// ---------------------------------------------------------------- perf

// 例: # perf ticks=30 period_max=34ms i2c_max=820us log_max=210us ...
//   ticks       ... 1 秒間のティック数 (30 前後なら正常)
//   period_max  ... ティック間隔の最大値 (33 前後なら正常。大きければ詰まっている)
//   i2c_max     ... センサ読み取りの最大所要時間
//   log_max     ... Serial.printf の最大所要時間 (ここが大きければシリアルが原因)
//   render_max  ... LED 描画の最大所要時間
//   log_dropped ... 送信バッファが詰まって捨てたログ行数 (1 秒ぶん)
//   drop_total  ... 同じく起動からの累計
//   tx_free     ... 送信バッファの空き (最大 256。0 付近のままなら PC 側が読んでいない)
//   last_ok     ... 最後に 1 行まるごと送れてからの経過時間
//                   (ログが止まった後でモニタを開き直したとき、いつ止まったかが分かる)
static void reportPerf(uint32_t nowMs)
{
    if (!ENABLE_PERF_LOG || nowMs - g_perfT < 1000) return;
    g_perfT = nowMs;

    // こちらも空きを見ずに毎秒必ず書き込みを試みる
    Serial.printf("# perf ticks=%lu period_max=%lums i2c_max=%luus "
                  "log_max=%luus render_max=%luus log_dropped=%lu i2c_err=%lu "
                  "drop_total=%lu tx_free=%d last_ok=%lums\n",
                  (unsigned long)g_ticks, (unsigned long)g_periodMax,
                  (unsigned long)g_tI2cMax, (unsigned long)g_tLogMax,
                  (unsigned long)g_tRenderMax, (unsigned long)g_logDropped,
                  (unsigned long)g_i2cErrors, (unsigned long)g_dropTotal,
                  Serial.availableForWrite(),
                  (unsigned long)(nowMs - g_lastLogOkMs));
    g_ticks = 0; g_periodMax = 0; g_tI2cMax = 0; g_tLogMax = 0;
    g_tRenderMax = 0; g_logDropped = 0; g_i2cErrors = 0;
}

// ---------------------------------------------------------------- loop

void loop()
{
    pollButton();

    if (!g_bmpReady) {
        static uint32_t last = 0;
        if (millis() - last >= 500) { last = millis(); ledError(); }
        delay(1);
        return;
    }

    uint32_t now = millis();
    if (now - g_lastTick < TICK_INTERVAL_MS) {
        // 次のティックまで CPU を眠らせる。これが無いと待ち時間のあいだ
        // millis() を回し続けて、CPU がずっとフル稼働になります。
        // ボタンも 1ms ごとに見られるので、操作性は変わりません
        delay(1);
        return;
    }

    uint32_t period = now - g_lastTick;
    if (period > g_periodMax) g_periodMax = period;
    g_ticks++;
    g_lastTick = now;

    // 読み取り -> ログ -> 描画 を 1 ティックで通す。
    // 間にバッファを挟まないので、センサの変化は最大 33ms で LED に出ます
    sampleOnce(now);

    if (++g_shiftCount >= TRAIL_STEP_TICKS) {
        g_shiftCount = 0;
        trailShift(g_head);
    }

    uint32_t tC = micros();
    switch (g_mode) {
        case 0: renderTrail();      break;
        case 1: showBar(g_lastHpa); break;
        default:                    break;   // 2 = 消灯
    }
    uint32_t tRender = micros() - tC;
    if (tRender > g_tRenderMax) g_tRenderMax = tRender;

    // 電池電圧は 1 分に 1 回。描画のあとに行うので、LED の更新には影響しない
    if (now - g_lastBatMs >= BATTERY_INTERVAL_MS) {
        g_lastBatMs = now;
        reportBattery();
    }

    reportPerf(now);
}
