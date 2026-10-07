#include "logging.h"

#include <Arduino.h>

#include "config.h"

// ---------------------------------------------------------------- SerialLog

void SerialLog::begin()
{
    Serial.begin(115200);
    // 送信バッファが埋まっても待たない (ログを落として LED を優先する)。
    // USB CDC On Boot = Enabled のとき Serial は HWCDC で、この関数があります
    Serial.setTxTimeoutMs(0);

    // USB CDC の接続待ち (繋がなくても先に進む)
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) delay(10);
}

bool SerialLog::writeLine(const char *line, size_t len, uint32_t nowMs)
{
    // 空きを見てから書くのではなく、毎回必ず書き込みを試みる。
    // 事前に弾くと書き込みが一切行われなくなり、HWCDC が送信割り込みを
    // 再開する処理 (write() の中にある) が動かず、ログが止まったままに
    // なることがある。待ち時間は 0 なので、入りきらなければ捨てるだけ
    uint32_t t0 = micros();
    size_t n = Serial.write((const uint8_t *)line, len);
    uint32_t us = micros() - t0;
    if (us > maxWriteUs_) maxWriteUs_ = us;

    if (n == len) {
        lastOkMs_ = nowMs;
        return true;
    }
    droppedRecent_++;
    droppedTotal_++;
    return false;
}

void SerialLog::resetRecent()
{
    droppedRecent_ = 0;
    maxWriteUs_    = 0;
}

// ---------------------------------------------------------------- PerfStats

void PerfStats::begin(uint32_t nowMs)
{
    lastReportMs_ = nowMs;
}

void PerfStats::tick(uint32_t periodMs)
{
    if (periodMs > periodMax_) periodMax_ = periodMs;
    ticks_++;
}

void PerfStats::i2c(uint32_t us)
{
    if (us > tI2cMax_) tI2cMax_ = us;
}

void PerfStats::i2cError()
{
    i2cErrors_++;
}

void PerfStats::render(uint32_t us)
{
    if (us > tRenderMax_) tRenderMax_ = us;
}

void PerfStats::report(uint32_t nowMs, SerialLog &log)
{
    if (!ENABLE_PERF_LOG || nowMs - lastReportMs_ < 1000) return;
    lastReportMs_ = nowMs;

    // こちらも空きを見ずに毎秒必ず書き込みを試みる
    Serial.printf("# perf ticks=%lu period_max=%lums i2c_max=%luus "
                  "log_max=%luus render_max=%luus log_dropped=%lu i2c_err=%lu "
                  "drop_total=%lu tx_free=%d last_ok=%lums\n",
                  (unsigned long)ticks_, (unsigned long)periodMax_,
                  (unsigned long)tI2cMax_, (unsigned long)log.maxWriteMicros(),
                  (unsigned long)tRenderMax_, (unsigned long)log.droppedRecent(),
                  (unsigned long)i2cErrors_, (unsigned long)log.droppedTotal(),
                  Serial.availableForWrite(),
                  (unsigned long)(nowMs - log.lastOkMs()));

    ticks_ = 0; periodMax_ = 0; tI2cMax_ = 0; tRenderMax_ = 0; i2cErrors_ = 0;
    log.resetRecent();
}
