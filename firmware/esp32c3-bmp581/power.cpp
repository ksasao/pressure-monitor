#include "power.h"

#include <Arduino.h>
#include <esp_sleep.h>

#include "config.h"
#include "boot.h"
#include "display.h"

// USB ホストが最後に見えた時刻 [ms]。0 = 起動から見えていない
static uint32_t s_lastHostSeenMs = 0;

// 最後にライトスリープから復帰した時刻 [ms]。この直後の isPlugged() は古い可能性がある
static uint32_t s_lastWakeMs = 0;

static bool     s_sleeping      = false;   // 直前のティックの待ちで、眠ったか
static uint8_t  s_failures      = 0;       // ライトスリープに失敗した連続回数
static bool     s_disabled      = false;   // 失敗が続いたので、無効にした

// 復帰の直後は、FreeRTOS のティックのフックが数回動くまで、isPlugged() が古い
static const uint32_t SETTLE_AFTER_WAKE_MS = 10;

static bool canSleep(uint32_t nowMs)
{
    if (!LIGHT_SLEEP_ENABLED || s_disabled) return false;
    if (bootIsSettingsMode())               return false;   // Wi-Fi が動いている
    if (nowMs < LIGHT_SLEEP_BOOT_DELAY_MS)  return false;   // 起動の直後
    if ((nowMs - s_lastHostSeenMs) < USB_UNPLUGGED_HOLD_MS) return false;   // USB が見えている
    if ((nowMs % USB_PROBE_INTERVAL_MS) < USB_PROBE_WINDOW_MS) return false;   // 検出窓
    if (!displayIsDark())                   return false;   // LED が点灯している間は眠らない
    return true;
}

void powerService(uint32_t nowMs)
{
    // 眠った直後の値は古いので、復帰から少したった「起きている間」だけ判定する
    if ((nowMs - s_lastWakeMs) < SETTLE_AFTER_WAKE_MS) return;

    if (Serial.isPlugged()) {
        if (s_sleeping || (nowMs - s_lastHostSeenMs) >= USB_UNPLUGGED_HOLD_MS) {
            // 接続を検出した (ここで眠るのをやめる)
            Serial.println(F("# USB host detected: light sleep off"));
        }
        s_lastHostSeenMs = nowMs;
        s_sleeping = false;
    }
}

bool powerHostPresent(uint32_t nowMs)
{
    return (nowMs - s_lastHostSeenMs) < USB_UNPLUGGED_HOLD_MS;
}

void powerIdle(uint32_t nowMs, uint32_t remainingMs)
{
    if (remainingMs >= LIGHT_SLEEP_MIN_MS && canSleep(nowMs)) {
        // LED への送信 (非同期) が終わってから眠る。送信の途中で眠ると、
        // データが壊れて、LED が意図しない色 (緑など) で光る
        displayWait();

        uint64_t us = (uint64_t)remainingMs * 1000ULL - LIGHT_SLEEP_WAKE_MARGIN_US;
        esp_sleep_enable_timer_wakeup(us);

        esp_err_t err = esp_light_sleep_start();
        s_lastWakeMs = millis();

        if (err == ESP_OK) {
            s_sleeping = true;
            s_failures = 0;
            displayNoteWake();      // 復帰後の最初の LED への送信は、2 回送る
            return;
        }

        // 眠れなかった。続くようなら、無効にする
        if (++s_failures >= 5) {
            s_disabled = true;
            Serial.printf("# light sleep disabled (err=%d)\n", (int)err);
        }
    }

    delay(1);       // CPU を止めて、次のティックを待つ
}
