// esp32c3-bmp581-basic
//
// 気圧ゆらぎセンサ開発ボード（ESP32-C3 + BMP581）の、基本の使い方のサンプルです。
// 次のことを、それぞれ短い関数にまとめています。
//
//   1. 気圧と温度を読む（BMP581、I2C）
//   2. BOOT ボタンの状態を読む
//   3. LED（WS2812B × 5）を点灯する
//   4. 電池電圧と、電源の状態（USB か電池か）を読む
//
// 動作: 起動したときの気圧を基準にして、そこからの変化を LED の本数で表示します。
//       気圧が上がる（ボードを下げる）と青、下がる（持ち上げる）とオレンジです。
//       BOOT ボタンを押すと、今の気圧を新しい基準にします（押している間は白く光ります）。
//       シリアルモニタ（115200bps）に、1 秒ごとに値を出します。
//
// 必要なライブラリ（ライブラリマネージャで入れます）
//   - SparkFun BMP581 Arduino Library
//   LED は、ESP32 のボードパッケージに入っている RMT の機能で点灯するので、
//   LED のライブラリ（FastLED など）は不要です。
//
// ボード設定: ESP32C3 Dev Module、USB CDC On Boot を Enabled にします。

#include <Wire.h>
#include <SparkFun_BMP581_Arduino_Library.h>

// ---- ピン割り当て（回路図 V1.1） ----
static const uint8_t PIN_SDA      = 4;   // BMP581 の SDI（Qwiic コネクタの SDA と共通）
static const uint8_t PIN_SCL      = 5;   // BMP581 の SCK（Qwiic コネクタの SCL と共通）
static const uint8_t PIN_BOOT     = 9;   // BOOT ボタン（押すと LOW）
static const uint8_t PIN_LED      = 10;  // LED1 の DIN（5 個がデイジーチェーン）
static const uint8_t PIN_VBAT     = 1;   // 電池電圧を 1/2 に分圧した電圧（ADC）
static const uint8_t PIN_POWER_ST = 3;   // 電源の状態（USB で HIGH、電池で LOW）。V1.1 の基板のみ

static const uint8_t NUM_LEDS     = 5;

// ---- 表示の調整 ----
static const float   PA_PER_LED   = 1.0f;  // LED 1 個あたりの気圧の変化 [Pa]（1 Pa は高さ約 8.5 cm）
static const uint8_t BRIGHTNESS   = 32;    // LED の明るさ（0〜255）。電池で使うときは小さめに

static BMP581 bmp;
struct Color { uint8_t r, g, b; };
static Color  leds[NUM_LEDS];
static float  referencePa = 0;             // 気圧の基準 [Pa]

// =====================================================================
// 1. 気圧と温度
// =====================================================================

// BMP581 を初期化する。I2C アドレスは 0x46（SDO が GND）。念のため 0x47 も試す
bool sensorBegin()
{
    Wire.begin(PIN_SDA, PIN_SCL);
    if (bmp.beginI2C(BMP581_I2C_ADDRESS_SECONDARY, Wire) != BMP5_OK &&  // 0x46（このボード）
        bmp.beginI2C(BMP581_I2C_ADDRESS_DEFAULT, Wire) != BMP5_OK) {    // 0x47
        return false;
    }

    // 測る間隔を 20Hz に、気圧のオーバーサンプリングを 32 倍にする（ノイズが小さくなる）
    bmp.setODRFrequency(BMP5_ODR_20_HZ);
    bmp5_osr_odr_press_config osr = {0};
    bmp.getOSRMultipliers(&osr);
    osr.osr_t    = BMP5_OVERSAMPLING_1X;
    osr.osr_p    = BMP5_OVERSAMPLING_32X;
    osr.press_en = BMP5_ENABLE;
    bmp.setOSRMultipliers(&osr);
    return true;
}

// 気圧 [Pa] と温度 [°C] を読む。読めたら true
bool sensorRead(float &pressurePa, float &temperatureC)
{
    bmp5_sensor_data data = {0, 0};
    if (bmp.getSensorData(&data) != BMP5_OK) {
        return false;
    }
    pressurePa   = data.pressure;      // Pa（温度補正済み）
    temperatureC = data.temperature;   // °C
    return true;
}

// =====================================================================
// 2. BOOT ボタン
// =====================================================================

// 押されている間は true。ボタンは押すと LOW になる
bool bootPressed()
{
    return digitalRead(PIN_BOOT) == LOW;
}

// =====================================================================
// 3. LED
// =====================================================================

// LED（WS2812B）を使えるようにする。RMT の 1 カウントを 0.1 µs にする
bool ledBegin()
{
    return rmtInit(PIN_LED, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);
}

// leds[] の色を、LED に送る。WS2812B は、緑・赤・青の順に、上位ビットから 24 ビットを受け取る
void ledShow()
{
    static rmt_data_t bits[NUM_LEDS * 24];
    int n = 0;
    for (int i = 0; i < NUM_LEDS; i++) {
        uint8_t grb[3] = {
            (uint8_t)(leds[i].g * BRIGHTNESS / 255),
            (uint8_t)(leds[i].r * BRIGHTNESS / 255),
            (uint8_t)(leds[i].b * BRIGHTNESS / 255),
        };
        for (int c = 0; c < 3; c++) {
            for (int b = 7; b >= 0; b--) {
                bool one = (grb[c] >> b) & 1;
                bits[n].level0    = 1;
                bits[n].duration0 = one ? 8 : 3;   // HIGH: 1 は 0.8 µs、0 は 0.3 µs
                bits[n].level1    = 0;
                bits[n].duration1 = one ? 4 : 9;   // LOW:  1 は 0.4 µs、0 は 0.9 µs
                n++;
            }
        }
    }
    rmtWrite(PIN_LED, bits, n, RMT_WAIT_FOR_EVER);
}

// 基準からの気圧の変化を、LED の本数で表示する
void showPressureChange(float deltaPa)
{
    Color color = (deltaPa >= 0) ? Color{0, 64, 255}     // 気圧が上がった: 青
                                 : Color{255, 96, 0};    // 気圧が下がった: オレンジ
    int count = (int)(fabsf(deltaPa) / PA_PER_LED + 0.5f);
    if (count > NUM_LEDS) count = NUM_LEDS;

    for (int i = 0; i < NUM_LEDS; i++) {
        leds[i] = (i < count) ? color : Color{0, 0, 0};
    }
    ledShow();
}

// 全ての LED を同じ色にする
void showAll(const Color &color)
{
    for (int i = 0; i < NUM_LEDS; i++) {
        leds[i] = color;
    }
    ledShow();
}

// =====================================================================
// 4. 電池電圧と電源の状態
// =====================================================================

// 電池電圧 [mV]。分圧（1/2）を戻して返す。
// 1000 mV 未満なら、電池が入っていないか、電源スイッチ（SW1）が OFF
uint32_t batteryMilliVolts()
{
    analogReadMilliVolts(PIN_VBAT);              // 1 回目は捨てる
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += analogReadMilliVolts(PIN_VBAT);
    }
    return sum / 16 * 2;
}

// USB から給電されていれば true、電池なら false。
// V1.0（2026-10-05 版）の基板は IO3 が未接続なので、この値は使えません
bool poweredByUsb()
{
    return digitalRead(PIN_POWER_ST) == HIGH;
}

// =====================================================================

void setup()
{
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);                    // シリアルモニタを開いていなくても、出力で待たない
    delay(1000);                                 // シリアルモニタが開くのを少し待つ

    pinMode(PIN_BOOT, INPUT_PULLUP);
    pinMode(PIN_POWER_ST, INPUT);                // 基板上で 1MΩ でプルアップ済み
    analogSetPinAttenuation(PIN_VBAT, ADC_11db); // 約 0〜2.5V を測れるようにする

    ledBegin();
    showAll(Color{0, 0, 0});

    if (!sensorBegin()) {
        Serial.println("BMP581 が見つかりません");
        while (true) {                           // 赤く点滅して止まる
            showAll(Color{255, 0, 0}); delay(200);
            showAll(Color{0, 0, 0});   delay(200);
        }
    }

    // 最初の値が安定するまで少し待ってから、基準にする
    delay(500);
    float t;
    sensorRead(referencePa, t);
    Serial.println("pressure_hPa,delta_Pa,temperature_C,boot,battery_mV,power");
}

void loop()
{
    static uint32_t lastPrintMs = 0;

    float pressurePa, temperatureC;
    if (!sensorRead(pressurePa, temperatureC)) {
        delay(50);
        return;
    }

    bool pressed = bootPressed();
    if (pressed) {
        referencePa = pressurePa;                // 押したら、今の気圧を基準にする
        showAll(Color{255, 255, 255});           // 押している間は白
    } else {
        showPressureChange(pressurePa - referencePa);
    }

    if (millis() - lastPrintMs >= 1000) {
        lastPrintMs = millis();
        uint32_t mv = batteryMilliVolts();
        Serial.printf("%.4f,%+.2f,%.2f,%s,%lu,%s\n",
                      pressurePa / 100.0f,
                      pressurePa - referencePa,
                      temperatureC,
                      pressed ? "pressed" : "released",
                      (unsigned long)mv,
                      poweredByUsb() ? "USB" : "battery");
    }

    delay(50);                                   // 約 20 回/秒
}
