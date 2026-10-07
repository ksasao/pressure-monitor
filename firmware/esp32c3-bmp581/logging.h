/*
 * logging.h - シリアル (USB CDC) への出力
 *
 *   SerialLog  1 行を出力する、汎用の窓口。待たずに書き、入りきらなければ捨てる。
 *              出力する内容 (CSV の列など) は、呼び出し側が決める。
 *              Qwiic に別のセンサを足したときも、同じ SerialLog に書ける。
 *   PerfStats  1 秒ごとの性能計測 ('#' で始まる行)
 *
 * CSV の例 (気圧):
 *   millis,pressure_hPa,raw,hpf,temperature_C,altitude_m,led_value
 *
 * 性能計測の例:
 *   # perf ticks=30 period_max=34ms i2c_max=820us log_max=210us ...
 *   ticks       1 秒間のティック数 (30 前後なら正常)
 *   period_max  ティック間隔の最大値 (33 前後なら正常。大きければ詰まっている)
 *   i2c_max     センサ読み取りの最大所要時間
 *   log_max     シリアルへの書き込みの最大所要時間 (大きければシリアルが原因)
 *   render_max  LED 描画の最大所要時間
 *   log_dropped 送信バッファが詰まって捨てたログ行数 (1 秒ぶん)
 *   drop_total  同じく起動からの累計
 *   tx_free     送信バッファの空き (最大 256。0 付近のままなら PC 側が読んでいない)
 *   last_ok     最後に 1 行まるごと送れてからの経過時間
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

class SerialLog {
public:
    // Serial の初期化。USB の接続は、最大 2 秒待つ (繋がなくても先に進む)
    void begin();

    // 1 行 (改行を含む) を書く。まるごと書けたら true。
    // 待ち時間は 0 なので、入りきらなければ、その行を捨てて false を返す
    bool writeLine(const char *line, size_t len, uint32_t nowMs);

    uint32_t droppedRecent() const  { return droppedRecent_; }   // 前回のリセット以降
    uint32_t droppedTotal() const   { return droppedTotal_; }    // 起動からの累計
    uint32_t maxWriteMicros() const { return maxWriteUs_; }      // 前回のリセット以降
    uint32_t lastOkMs() const       { return lastOkMs_; }        // 最後に成功した時刻

    void resetRecent();     // 「前回のリセット以降」の値を 0 に戻す

private:
    uint32_t droppedRecent_ = 0;
    uint32_t droppedTotal_  = 0;
    uint32_t maxWriteUs_    = 0;
    uint32_t lastOkMs_      = 0;
};

class PerfStats {
public:
    void begin(uint32_t nowMs);

    void tick(uint32_t periodMs);   // ティックの開始時に呼ぶ
    void i2c(uint32_t us);          // センサ読み取りの所要時間 [µs]
    void i2cError();
    void render(uint32_t us);       // LED 描画の所要時間 [µs]

    // 1 秒たっていれば、1 行出力して、計測値をリセットする
    void report(uint32_t nowMs, SerialLog &log);

private:
    uint32_t tI2cMax_    = 0;
    uint32_t tRenderMax_ = 0;
    uint32_t periodMax_  = 0;
    uint32_t ticks_      = 0;
    uint32_t i2cErrors_  = 0;
    uint32_t lastReportMs_ = 0;
};
