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

// 1 回読む。i2cMicros には、読み取りにかかった時間 [µs] が入る (失敗時も)
SensorStatus sensorRead(SensorSample &out, uint32_t &i2cMicros);
