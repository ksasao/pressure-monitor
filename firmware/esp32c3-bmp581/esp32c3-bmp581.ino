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
 *   - "ESP Async WebServer"  (ESP32Async 製。設定モードの Web サーバ)
 *   - "Async TCP"            (ESP32Async 製)
 *
 * ボード設定 (Arduino IDE)
 *   Board            : ESP32C3 Dev Module
 *   USB CDC On Boot  : Enabled      <-- Disabled だと Serial が出ません
 *   Partition Scheme : Huge APP (3MB No OTA/1MB SPIFFS)
 *                                   <-- 既定 (1.2MB APP) だと、容量の 9 割近くになります
 *   Flash Size       : 4MB (32Mb)   <-- 既定のまま
 *
 * --- 動作 ---
 * 約 30Hz (33ms 周期) で、センサの読み取り -> HPF -> 色 -> LED 表示 -> ログ を
 * 1 本のティックで通します。間にバッファを挟まないので、センサの変化は
 * 最大 33ms で LED に出ます。平滑化フィルタは HPF だけです。
 * 気圧の値 (1 LSB = 1/64 Pa) を、整数のまま 1 次ハイパスフィルタに通し、
 * 変化の向きと大きさを、LED の色と明るさにします
 * (high_pass_filter.h、palette.h)。
 *
 * --- 設定モード (Wi-Fi) ---
 * BOOT ボタンを 1.5 秒以上長押しして離すと、再起動して設定モードになります。
 * Wi-Fi のアクセスポイントを立ち上げ、スマホから感度などを変更できます (portal.h)。
 *   SSID / パスワード : config.h の AP_SSID / AP_PASSWORD (既定はパスワードなし)
 *   アドレス           : http://192.168.4.1/
 * 設定ページの「保存して終了」、もう一度の長押し、または一定時間の無操作で、
 * 再起動して通常モードに戻ります。通常モードでは Wi-Fi を初期化しないので、
 * 無線は完全にオフです。
 *
 * --- ファイル構成 ---
 *   esp32c3-bmp581.ino  setup() と loop()。各モジュールをつなぐ
 *   config.h            調整できる定数、ピン割り当て
 *   settings.*          設定値 (設定モードで変更・保存できる値)
 *   boot.*              リセットの理由、通常モード / 設定モードの切り替え
 *   sensor.*            気圧センサ BMP581
 *   high_pass_filter.*  ハイパスフィルタ (class HighPassFilter)
 *   modes.*             表示モード (標準 / ドライブ / カスタム / 固定色) と、感度
 *   palette.*           変化量から色への変換、HSV から色への変換
 *   display.*           LED の表示
 *   button.*            ボタン (class Button。短押し / 長押し)
 *   battery.*           電池電圧
 *   logging.*           シリアルへの出力 (class SerialLog) と性能計測 (class PerfStats)
 *   power.*             省電力 (USB が接続されていないときの、ライトスリープ)
 *   settings_page.*     設定ページの HTML と、状況の JSON
 *   portal.*            設定モードの Wi-Fi / DNS / Web サーバ
 *   led_types.h         FastLED の型を使うためのヘッダ
 */

#include <Arduino.h>

#include "config.h"
#include "settings.h"
#include "boot.h"
#include "sensor.h"
#include "high_pass_filter.h"
#include "palette.h"
#include "modes.h"
#include "display.h"
#include "button.h"
#include "battery.h"
#include "logging.h"
#include "portal.h"
#include "power.h"

// 表示モード (modes.h): 0 = 標準, 1 = ドライブ, 2 = カスタム, 3 = 固定色。電源を切っても残る
// 今のモードは g_set.mode が持つ (設定ページからも変えられるため、ここでは複製しない)
static inline uint8_t currentMode() { return (uint8_t)g_set.mode; }
static bool     g_modeDirty = false;       // モードを切り替えたが、まだ保存していない

// BOOT ボタンを押したときの、モード番号の表示時間 [ms]
static const uint32_t MODE_NUMBER_MS = 2000;

static bool     g_sensorReady  = false;
static uint8_t  g_shiftCount   = 0;
static bool     g_holdFeedback = false;    // BOOT 長押し中 (描画を止めて合図を出す)

static uint32_t g_lastTick  = 0;
static uint32_t g_lastBatMs = 0;           // 最後に電池電圧を出力した時刻

// 機能ごとのインスタンス。
// Qwiic に別のセンサを足すときは、センサごとに HighPassFilter を作り、
// 同じ SerialLog に、そのセンサの CSV の行を書きます
static HighPassFilter g_pressureHpf;                    // 気圧のハイパスフィルタ
static Button         g_bootButton(PIN_BOOT, LONG_PRESS_MS);
static SerialLog      g_log;
static PerfStats      g_perf;

// BOOT ボタン (どちらも、ボタンを離したときに動作する)
//   短押し : 今のモードの番号を、番号の数だけ LED を青で点灯して知らせる (2 秒間)。
//            その 2 秒の間に、続けて押すと、次のモードに切り替わる (4 の次は 1)
//   長押し : 設定モード (Wi-Fi) と通常モードを切り替え
//
// 切り替えは再起動で行う。ボタンを押したまま再起動すると、IO9 が Low のまま
// リセットされて、書き込みモード (ダウンロードモード) に入ってしまうため、
// 必ず離してから再起動する。
// 長押しが成立した (1.5 秒たった) ときは、LED を水色にして合図する
static void handleButton()
{
    switch (g_bootButton.poll()) {
        case BTN_LONG_REACHED:
            g_holdFeedback = true;              // 描画を止めて、水色のままにする
            displayFill(CRGB(0, 50, 50));
            break;

        case BTN_LONG_RELEASED:
            g_holdFeedback = false;
            bootRebootInto(!bootIsSettingsMode());
            break;

        case BTN_SHORT:
            g_holdFeedback = false;
            // 番号の表示中なら次のモードへ。表示していなければ、今の番号を見せるだけ
            if (displayModeNumberActive()) {
                g_set.mode = (g_set.mode + 1) % MODE_COUNT;
                g_modeDirty = true;
                Serial.printf("# mode -> %u (%s)\n", currentMode() + 1, modeName(currentMode()));
            }
            displayShowModeNumber(currentMode() + 1, MODE_NUMBER_MS);
            break;

        default:
            break;
    }
}

// 気圧の CSV を 1 行出す (LOG_EVERY_N ティックに 1 回)
//   millis,pressure_hPa,raw,hpf,temperature_C,altitude_m,led_value,battery_mV
// battery_mV は最後に測った電池電圧 (BATTERY_INTERVAL_MS ごとに測る)。毎行、その値を出す
static void logPressure(uint32_t nowMs, const SensorSample &s, int32_t hpf, uint8_t ledValue)
{
    static uint8_t count = 0;
    if (++count < LOG_EVERY_N) return;
    count = 0;

    char buf[96];
    int len = snprintf(buf, sizeof(buf), "%lu,%.3f,%ld,%ld,%.2f,%.2f,%u,%lu\n",
                       (unsigned long)nowMs, s.hPa, (long)s.raw, (long)hpf,
                       s.tempC, s.altM, ledValue, (unsigned long)batteryLastMilliVolts());
    if (len > (int)sizeof(buf) - 1) len = sizeof(buf) - 1;   // 切り詰められた場合の保険

    g_log.writeLine(buf, (size_t)len, nowMs);
}

// センサを 1 回読んで HPF と LED1 の色を更新し、ログを出す。
// logEnabled が false (USB ホストが見えない) ときは、誰も読まないので、ログを出さない
static void sampleOnce(uint32_t nowMs, bool logEnabled)
{
    SensorSample s;
    uint32_t i2cUs = 0;
    SensorStatus st = sensorRead(s, i2cUs);

    g_perf.i2c(i2cUs);
    if (st == SENSOR_I2C_ERROR) g_perf.i2cError();
    if (st != SENSOR_OK) return;

    portalSetSensor(s.hPa, s.tempC);            // 設定ページの表示用

    const SenseParams p = modeSense(currentMode());
    int32_t hpf   = g_pressureHpf.update(s.raw, p.hpfShift);
    CRGB    color = deltaToColor(hpf, p.deltaLimit, p.deltaRange);

    // 平滑化せずそのまま反映する。ここが応答性の要です。
    // 固定色モードでは使わない。履歴を消しておく (残ると、眠れなくなる)
    if (modeIsSolid(currentMode())) displayClearTrail();
    else                     displaySetHead(color);

    if (logEnabled) logPressure(nowMs, s, hpf, colorPeak(color));
}

// ---------------------------------------------------------------- setup

void setup()
{
    // 再起動の理由と、設定モードへ入る指示かどうかを、最初に確認する
    bootDetect();

    // CPU クロックは、Serial / I2C / FastLED の初期化より前に決める。
    // Wi-Fi は 80MHz 以上が必要なので、設定モードのときだけ上げる
    setCpuFrequencyMhz(bootIsSettingsMode() ? SETTINGS_CPU_MHZ : CPU_MHZ);

    // 電源が入ったら、できるだけ速やかに、LED を白で全点灯する。
    // 明るさの設定だけは必要なので、先に読む (数 ms)。シリアルの接続待ち (最大 2 秒) などの
    // 前に点灯することで、電源が入ったことをすぐに知らせる
    settingsLoad();                 // 保存された設定 (なければ既定値)
    displayBegin((uint8_t)g_set.brightness);
    const uint32_t whiteStartMs = millis();

    g_log.begin();

    Serial.println();
    Serial.println(F("=== PressureMonitor bring-up (FastLED) ==="));
    Serial.printf("# reset reason: %s%s\n", bootResetReasonName(),
                  bootIsSettingsMode() ? " -> settings mode" : "");

    // 電源の落ち込みによるリセットは、累計を残しておく (設定ページで確認できる)
    if (bootWasBrownout()) {
        settingsAddBrownout();
        Serial.printf("# brownout reset detected (total %lu)\n",
                      (unsigned long)settingsBrownoutCount());
    }

    g_bootButton.begin();

    // 白の点灯を、最低でも BOOT_WHITE_MS は見せてから、自己診断に進む
    while ((millis() - whiteStartMs) < BOOT_WHITE_MS) delay(10);
    displaySelfTest();

    // 設定モードは、センサの有無にかかわらず Wi-Fi を起動する
    // (Wi-Fi が回路で動くかの確認が目的のため)
    if (bootIsSettingsMode()) portalBegin();

    g_sensorReady = sensorBegin();
    if (g_sensorReady) {
        const SenseParams p = modeSense(currentMode());
        Serial.printf("# mode %u (%s)\n", currentMode() + 1, modeName(currentMode()));
        Serial.printf("# tick %lu ms (%lu Hz) / HPF shift %ld"
                      " / limit %ld range %ld (1/64 Pa)\n",
                      (unsigned long)TICK_INTERVAL_MS,
                      1000UL / TICK_INTERVAL_MS,
                      (long)p.hpfShift,
                      (long)p.deltaLimit, (long)p.deltaRange);
        Serial.printf("# cpu %lu MHz\n", (unsigned long)getCpuFrequencyMhz());
        batteryMeasure();           // 起動時に 1 回。以降は 1 分ごと
        Serial.println(F("ready."));
        Serial.println(F("millis,pressure_hPa,raw,hpf,"
                         "temperature_C,altitude_m,led_value,battery_mV"));
    }

    uint32_t now = millis();
    g_lastTick  = now;
    g_lastBatMs = now;
    g_perf.begin(now);
}

// ---------------------------------------------------------------- loop

void loop()
{
    handleButton();

    // モード番号の表示が終わったら、選んだモードを保存する
    // (押すたびに書かず、フラッシュの書き込みを減らす)
    if (g_modeDirty && !displayModeNumberActive()) {
        g_modeDirty = false;
        settingsSaveMode();
    }

    powerService(millis());         // USB ホストが見えているかを調べる

    // 設定モード: DNS・電池電圧の監視・タイムアウト。
    // センサが見つからない場合でも動かす
    if (bootIsSettingsMode()) portalService(millis());

    if (!g_sensorReady) {
        static uint32_t last = 0;
        if (millis() - last >= 500) { last = millis(); displayError(); }
        delay(1);
        return;
    }

    uint32_t now = millis();
    if (now - g_lastTick < TICK_INTERVAL_MS) {
        // 次のティックまで待つ。これが無いと待ち時間のあいだ millis() を回し続けて、
        // CPU がずっとフル稼働になります。
        // USB が接続されていないときは、ライトスリープで過ごします (power.h)。
        // そうでないときは delay(1) で CPU を止めるので、ボタンも 1ms ごとに見られます
        powerIdle(now, TICK_INTERVAL_MS - (now - g_lastTick));
        return;
    }

    uint32_t period = now - g_lastTick;
    g_perf.tick(period);
    g_lastTick = now;

    // 読み取り -> ログ -> 描画 を 1 ティックで通す。
    // 間にバッファを挟まないので、センサの変化は最大 33ms で LED に出ます
    const bool hostPresent = powerHostPresent(now);     // USB ホストが見えるか
    sampleOnce(now, hostPresent);

    if ((int32_t)(++g_shiftCount) >= modeSense(currentMode()).trailStepTicks) {
        g_shiftCount = 0;
        displayTrailShift();
    }

    uint32_t tC = micros();
    if (!g_holdFeedback) {              // 長押しの合図 (水色) を消さない
        // 設定モード中は、最後の LED を状態表示にする
        //   暗い水色 : Wi-Fi は起動していて、スマホの接続を待っている
        //   緑       : スマホが接続している
        if (bootIsSettingsMode()) {
            displaySetStatus(true, portalHasClient() ? CRGB(0, 60, 0) : CRGB(0, 40, 40));
        }
        if (modeIsSolid(currentMode())) {
            displayRenderSolid(hsvToColor(g_set.hue, g_set.sat, g_set.val));
        } else {
            displayRenderTrail((uint8_t)g_set.brightness);
        }
    }
    g_perf.render(micros() - tC);

    // 電池電圧は 1 分に 1 回。描画のあとに行うので、LED の更新には影響しない
    if (now - g_lastBatMs >= BATTERY_INTERVAL_MS) {
        g_lastBatMs = now;
        batteryMeasure();
    }

    if (hostPresent) g_perf.report(now, g_log);
}
