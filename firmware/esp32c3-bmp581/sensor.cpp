#include "sensor.h"

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <SparkFun_BMP581_Arduino_Library.h>

#include "config.h"

static BMP581  s_bmp;
static uint8_t s_addr = 0;

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
        if (s_bmp.beginI2C(candidates[i], Wire) == BMP5_OK) {
            s_addr = candidates[i];
            Serial.printf("BMP581 found at 0x%02X\n", s_addr);
            if (s_addr == ADDR_SDO_HIGH) {
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
    int8_t err = s_bmp.setODRFrequency(BMP5_ODR_50_HZ);
    if (err != BMP5_OK) {
        Serial.printf("setODRFrequency failed (%d)\n", err);
        return false;
    }

    bmp5_osr_odr_press_config cfg = {0};
    err = s_bmp.getOSRMultipliers(&cfg);
    if (err != BMP5_OK) {
        Serial.printf("getOSRMultipliers failed (%d)\n", err);
        return false;
    }

    cfg.osr_t    = BMP5_OVERSAMPLING_1X;
    cfg.osr_p    = BMP5_OVERSAMPLING_16X;
    cfg.press_en = BMP5_ENABLE;

    err = s_bmp.setOSRMultipliers(&cfg);
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
        s_bmp.getSensorData(&scratch);
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

bool sensorBegin()
{
    Wire.begin(PIN_SDA, PIN_SCL, 400000);

    i2cScan();

    if (!bmpBegin()) {
        Serial.println(F("BMP581 not responding."));
        Serial.println(F("  確認項目: CSB が VDDIO にプルアップされているか "
                         "(GND だと SPI モードになり I2C から見えません)"));
        return false;
    }

    if (!bmpConfigure()) {
        Serial.println(F("BMP581 configuration failed."));
        return false;
    }

    bmpWarmUp();
    return true;
}

SensorStatus sensorRead(SensorSample &out, uint32_t &i2cMicros)
{
    bmp5_sensor_data data = {0, 0};

    uint32_t t0 = micros();
    int8_t err = s_bmp.getSensorData(&data);
    i2cMicros = micros() - t0;

    if (err != BMP5_OK) {
        Serial.printf("# getSensorData failed (%d)\n", err);
        return SENSOR_I2C_ERROR;
    }

    if (!isPlausible(data)) {
        Serial.printf("# discarded: %.2f hPa, %.2f degC\n",
                      data.pressure / 100.0f, data.temperature);
        return SENSOR_IMPLAUSIBLE;
    }

    out.hPa   = data.pressure / 100.0f;     // ライブラリは Pa で返す
    out.tempC = data.temperature;
    out.altM  = toAltitude(out.hPa);

    // BMP581 の気圧レジスタは 1 LSB = 1/64 Pa。float は Pa x 64 を
    // 厳密に表せる (2 のべき乗倍) ので、整数に戻しても情報は落ちません
    out.raw = (int32_t)lroundf(data.pressure * 64.0f);

    return SENSOR_OK;
}
