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

// センサが 30Hz で測り、値を FIFO にためる設定。
//
// 基板は約 33ms ごとに起きて、FIFO にたまった値を全て読む (たいてい 1 個、ときどき 0 個か 2 個)。
// センサが測った値を、重複も読み落としもなく、1 回ずつ処理できる。
// 30Hz なら、オーバーサンプリングは 32X まで使える (64X 以上は、センサが黙って 4X に下げる)。
// 1 サンプルのノイズは約 0.14 Pa (50Hz・16X では約 0.18 Pa)。
// 温度は補正と表示にしか使わないので 1X まで削って、その時間を気圧に回します。
// センサの 30Hz は、内蔵の発振器で決まり、実測で約 +0.09% 速い (時刻は devlog.cpp で補正する)
//
// [!] BMP5_ODR_30_HZ / BMP5_OVERSAMPLING_* のマクロ名はライブラリの
//     バージョンで異なることがあります。コンパイルが通らない場合は
//     bmp5_defs.h の定義を確認してください。
static bool bmpConfigure()
{
    int8_t err = s_bmp.setODRFrequency(BMP5_ODR_30_HZ);
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
    cfg.osr_p    = BMP5_OVERSAMPLING_32X;
    cfg.press_en = BMP5_ENABLE;

    err = s_bmp.setOSRMultipliers(&cfg);
    if (err != BMP5_OK) {
        Serial.printf("setOSRMultipliers failed (%d)\n", err);
        return false;
    }

    // FIFO: 気圧と温度の組 (6 バイト) を、32 個までためる。いっぱいになると、古いものから捨てる
    bmp5_fifo fifo = {0};
    fifo.frame_sel      = BMP5_FIFO_PRESS_TEMP_DATA;
    fifo.dec_sel        = BMP5_FIFO_NO_DOWNSAMPLING;
    fifo.mode           = BMP5_FIFO_MODE_STREAMING;
    fifo.threshold      = 0;
    fifo.set_fifo_iir_t = BMP5_DISABLE;
    fifo.set_fifo_iir_p = BMP5_DISABLE;
    err = s_bmp.setFIFOConfig(&fifo);
    if (err != BMP5_OK) {
        Serial.printf("setFIFOConfig failed (%d)\n", err);
        return false;
    }
    return true;
}

// 設定直後はデータレジスタにリセット値が残っており、そのまま読むと
// 1305 hPa / 127.5 degC のような値が出る。最初の変換が終わるまで捨てる
// FIFO も、最初の変換が終わってから空にする
static void bmpWarmUp()
{
    delay(150);
    bmp5_sensor_data scratch = {0, 0};
    for (uint8_t i = 0; i < 5; i++) {
        s_bmp.getSensorData(&scratch);
        delay(30);
    }
    bmp5_osr_odr_eff eff = {0, 0, 0};
    if (s_bmp.getOSREffective(&eff) == BMP5_OK) {
        Serial.printf("# BMP581: ODR 30 Hz / OSR %ux%s / FIFO\n", 1u << eff.osr_p_eff,
                      eff.odr_is_valid ? "" : " (invalid combination)");
    }
    s_bmp.flushFIFO();
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

// FIFO から読んだ 1 個を、SensorSample にする
static void toSample(const bmp5_sensor_data &d, SensorSample &out)
{
    out.hPa   = d.pressure / 100.0f;        // ライブラリは Pa で返す
    out.tempC = d.temperature;
    out.altM  = toAltitude(out.hPa);

    // BMP581 の気圧レジスタは 1 LSB = 1/64 Pa。float は Pa x 64 を
    // 厳密に表せる (2 のべき乗倍) ので、整数に戻しても情報は落ちません
    out.raw = (int32_t)lroundf(d.pressure * 64.0f);
}

uint8_t sensorReadFifo(SensorSample *out, uint8_t maxCount, uint32_t &i2cMicros, SensorFifoStatus &st)
{
    st = { SENSOR_OK, false, false };
    const uint32_t t0 = micros();
    uint8_t n = 0;
    int8_t err = s_bmp.getFIFOLength(&n);
    if (err == BMP5_OK && n > 0) {
        if (n >= SENSOR_FIFO_DEPTH) st.overflow = true;     // いっぱい: 古いものを捨てたかもしれない
        if (n > SENSOR_FIFO_DEPTH) n = SENSOR_FIFO_DEPTH;
        bmp5_sensor_data data[SENSOR_FIFO_DEPTH];
        err = s_bmp.getFIFOData(data, n);
        if (err == BMP5_OK) {
            uint8_t k = 0;
            // 読み切れないほどたまっていたら (起きない想定)、新しい方を残す
            const uint8_t from = n > maxCount ? n - maxCount : 0;
            if (from > 0) st.dropped = true;
            for (uint8_t i = from; i < n; i++) {
                if (!isPlausible(data[i])) {
                    Serial.printf("# discarded: %.2f hPa, %.2f degC\n",
                                  data[i].pressure / 100.0f, data[i].temperature);
                    st.status  = SENSOR_IMPLAUSIBLE;
                    st.dropped = true;
                    continue;
                }
                toSample(data[i], out[k++]);
            }
            n = k;
        }
    }
    i2cMicros = micros() - t0;
    if (err != BMP5_OK) {
        Serial.printf("# FIFO read failed (%d)\n", err);
        st.status  = SENSOR_I2C_ERROR;
        st.dropped = true;
        return 0;
    }
    return n;
}
