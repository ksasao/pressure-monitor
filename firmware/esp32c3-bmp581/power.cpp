#include "power.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include "config.h"
#include "boot.h"
#include "display.h"

// IO3 (TPS2116 の ST) の状態。V1.0 の基板は IO3 が未接続なので、使わない
static bool s_hasPowerSt = false;

// USB ホストが最後に見えた時刻 [ms]。0 = 起動から見えていない
static uint32_t s_lastHostSeenMs = 0;

// 最後にライトスリープから復帰した時刻 [ms]。この直後の isPlugged() は古い可能性がある
static uint32_t s_lastWakeMs = 0;

static bool     s_sleeping      = false;   // 直前のティックの待ちで、眠ったか
static uint8_t  s_failures      = 0;       // ライトスリープに失敗した連続回数
static bool     s_disabled      = false;   // 失敗が続いたので、無効にした
static bool     s_keepAwake     = false;   // 無線を使っている (送信・ハブ)

// 復帰の直後は、FreeRTOS のティックのフックが数回動くまで、isPlugged() が古い
static const uint32_t SETTLE_AFTER_WAKE_MS = 10;

// IO3 を、内蔵のプルアップ・プルダウンで読む
static int readPowerStWithPull(gpio_pull_mode_t pull)
{
    gpio_set_pull_mode((gpio_num_t)PIN_POWER_ST, pull);
    delayMicroseconds(200);
    const int v = digitalRead(PIN_POWER_ST);
    gpio_set_pull_mode((gpio_num_t)PIN_POWER_ST, GPIO_FLOATING);
    return v;
}

void powerBegin()
{
    // IO3 がつながっているか (V1.1) を見分ける。ST は、電池のときは Low に引かれ、USB のときは
    // R13 (1MΩ) で High になる。未接続 (V1.0) なら、内蔵のプルに従って、どちらにも変わる。
    //   内蔵のプルダウンを外した直後に High   -> USB から給電 (V1.1)
    //   内蔵のプルアップでも Low               -> 電池から給電 (V1.1)
    //   プルアップで High、プルダウンで Low    -> 未接続 (V1.0)
    pinMode(PIN_POWER_ST, INPUT);
    const int up = readPowerStWithPull(GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)PIN_POWER_ST, GPIO_PULLDOWN_ONLY);
    delayMicroseconds(200);
    gpio_set_pull_mode((gpio_num_t)PIN_POWER_ST, GPIO_FLOATING);
    delayMicroseconds(100);             // 1MΩ x 数 pF で、数 µs で High に戻る
    const int down = digitalRead(PIN_POWER_ST);
    s_hasPowerSt = (down == HIGH) || (up == LOW);

    if (s_hasPowerSt) {
        // 眠っている間に USB の給電が始まったら、すぐに起きる (PC からの接続に間に合わせる)
        gpio_wakeup_enable((gpio_num_t)PIN_POWER_ST, GPIO_INTR_HIGH_LEVEL);
        esp_sleep_enable_gpio_wakeup();
    }
    Serial.printf("# power: ST (IO3) %s, %s\n", s_hasPowerSt ? "found" : "not connected (V1.0)",
                  powerUsbPowered() ? "USB" : "battery");
}

bool powerUsbPowered()
{
    return s_hasPowerSt && digitalRead(PIN_POWER_ST) == HIGH;
}

static bool canSleep(uint32_t nowMs)
{
    if (!LIGHT_SLEEP_ENABLED || s_disabled) return false;
    if (powerUsbPowered())                  return false;   // USB から給電 (PC への接続に備える)
    if (bootIsSettingsMode())               return false;   // Wi-Fi が動いている
    if (s_keepAwake)                        return false;   // ESP-NOW / ハブが動いている
    if (nowMs < LIGHT_SLEEP_BOOT_DELAY_MS)  return false;   // 起動の直後
    if ((nowMs - s_lastHostSeenMs) < USB_UNPLUGGED_HOLD_MS) return false;   // USB が見えている
    if ((nowMs % USB_PROBE_INTERVAL_MS) < USB_PROBE_WINDOW_MS) return false;   // 検出窓
    if (!LIGHT_SLEEP_WHILE_LIT && !displayIsDark()) return false;   // 点灯中は眠らない (以前の動作)
    return true;
}

void powerKeepAwake()
{
    s_keepAwake = true;
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
