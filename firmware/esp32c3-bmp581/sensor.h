/*
 * sensor.h - 気圧センサ BMP581 (I2C)
 */
#pragma once

#include <stdint.h>

struct SensorSample {
    float   hPa;        // 気圧 [hPa]
    float   tempC;      // 温度 [degC]
    int32_t raw;        // 気圧の値 (1 LSB = 1/64 Pa)。BMP581 の内蔵温度補正済みの値
    float   altM;       // 標準大気 (SEA_LEVEL_HPA) を基準にした標高 [m]
};

enum SensorStatus {
    SENSOR_OK,
    SENSOR_I2C_ERROR,    // 読み取りに失敗した
    SENSOR_IMPLAUSIBLE   // 値が異常 (I2C が化けた場合など)。捨てる
};

// I2C の初期化、センサの検出、設定、ウォームアップ。成功したら true。
// 結果は、シリアルに出力する
bool sensorBegin();

// センサの FIFO の大きさ (30Hz で約 1 秒分)
static const uint8_t SENSOR_FIFO_DEPTH = 32;

struct SensorFifoStatus {
    SensorStatus status;    // 最後の問題 (なければ SENSOR_OK)
    bool overflow;          // FIFO がいっぱいだった (古い値を、センサが捨てたかもしれない)
    bool dropped;           // 読めなかった値・捨てた値がある (サンプルが抜けた)
};

// FIFO にたまった値を全て読む (センサは 30Hz で測る)。古い順に out に入れ、個数を返す (0 もある)。
// i2cMicros には、読み取りにかかった時間 [µs] が入る (失敗時も)
uint8_t sensorReadFifo(SensorSample *out, uint8_t maxCount, uint32_t &i2cMicros, SensorFifoStatus &st);
