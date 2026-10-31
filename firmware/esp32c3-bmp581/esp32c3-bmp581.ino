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
 *   Partition Scheme : No OTA (2MB APP/2MB SPIFFS)
 *                                   <-- 既定 (1.2MB APP) だと、プログラムが入りません。
 *                                       SPIFFS の領域 (約 1.9MB) を、基板の中への記録に使います (devlog.h)
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
 * 再起動して通常モードに戻ります。通常モードでは、役割が「単体」なら Wi-Fi を初期化しないので、
 * 無線は完全にオフです。
 *
 * --- 複数台の表示 (ESP-NOW) ---
 * 設定ページで、役割を「送信」「ハブ」にすると、送信の装置の気圧のデータを、ハブが集めて、
 * スマホのブラウザに表示します (link.h、hub.h)。
 *
 * --- ファイル構成 ---
 *   esp32c3-bmp581.ino  setup() と loop()。各モジュールをつなぐ
 *   config.h            調整できる定数、ピン割り当て
 *   version.h           ファームウェアのバージョン (リリース日。リリースのスクリプトが書き換える)
 *   settings.*          設定値 (設定モードで変更・保存できる値)
 *   boot.*              リセットの理由、通常モード / 設定モードの切り替え
 *   sensor.*            気圧センサ BMP581
 *   high_pass_filter.*  ハイパスフィルタ (class HighPassFilter)
 *   modes.*             表示モード (標準 / ドライブ / カスタム / 固定色) と、感度
 *   palette.*           変化量から色への変換、HSV から色への変換
 *   display.*           LED の表示
 *   button.*            ボタン (class Button。短押し / 長押し)
 *   battery.*           電池電圧
 *   sound.*             音 (Qwiic の Modulino Buzzer。LED の色の変化に合わせて鳴らす)
 *   logging.*           シリアルへの出力 (class SerialLog) と性能計測 (class PerfStats)
 *   power.*             省電力 (USB が接続されていないときの、ライトスリープ)
 *   settings_page.*     設定ページの HTML と、状況の JSON
 *   portal.*            設定モードの Wi-Fi / DNS / Web サーバ
 *   monitor_page.*      設定モードで最初に開くページ (気圧の絶対値、変化量、グラフ)
 *   devlog.*            基板の中への記録 (フラッシュ。表示ページから操作し、CSV でダウンロード)
 *   link.*              複数台の表示。ESP-NOW の送信と、パケットの形式
 *   hub.*               ハブ (受信したデータを、スマホのブラウザへ送る)
 *   hub_page.*          ハブの表示ページ (HTML とスクリプト)
 *   led_types.h         FastLED の型を使うためのヘッダ
 */

#include <Arduino.h>

#include "config.h"
#include "version.h"
#include "settings.h"
#include "boot.h"
#include "sensor.h"
#include "high_pass_filter.h"
#include "palette.h"
#include "modes.h"
#include "display.h"
#include "button.h"
#include "battery.h"
#include "sound.h"
#include "logging.h"
#include "portal.h"
#include "power.h"
#include "link.h"
#include "hub.h"
#include "devlog.h"

// 表示モード (modes.h): 0 = 標準, 1 = ドライブ, 2 = カスタム, 3 = 固定色。電源を切っても残る
// 今のモードは g_set.mode が持つ (設定ページからも変えられるため、ここでは複製しない)
static inline uint8_t currentMode() { return (uint8_t)g_set.mode; }
static bool     g_modeDirty = false;       // モードを切り替えたが、まだ保存していない

// BOOT ボタンを押したときの、モード番号の表示時間 [ms]
static const uint32_t MODE_NUMBER_MS = 2000;

// 固定色モードの、フィルタの出力 (-1000〜1000。標準モードの感度)。明度に反映する
static int32_t  g_solidLevel   = 0;

static bool     g_sensorReady  = false;
static uint8_t  g_shiftCount   = 0;
static bool     g_holdFeedback = false;    // BOOT 長押し中 (描画を止めて合図を出す)

static uint32_t g_lastTick  = 0;
static uint32_t g_lastBatMs = 0;           // 最後に電池電圧を出力した時刻

// 機能ごとのインスタンス。
// Qwiic に別のセンサを足すときは、センサごとに HighPassFilter を作り、
// 同じ SerialLog に、そのセンサの CSV の行を書きます
static HighPassFilter g_pressureHpf;                    // 気圧のハイパスフィルタ
static int32_t  g_smoothAcc    = 0;                     // 表示に使う変化量 x 256 (ドライブで保持する)
static Button         g_bootButton(PIN_BOOT, LONG_PRESS_MS, FACTORY_RESET_PRESS_MS);
static SerialLog      g_log;
static PerfStats      g_perf;

// BOOT ボタン (どちらも、ボタンを離したときに動作する)
//   短押し : 今のモードの番号を、番号の数だけ LED を青で点灯して知らせる (2 秒間)。
//            その 2 秒の間に、続けて押すと、次のモードに切り替わる (4 の次は 1)
//   長押し : 設定モード (Wi-Fi) と通常モードを切り替え
//   20 秒押し続ける : 全ての LED を赤で 1 秒間点滅したあと、設定を全て初期値に戻して、再起動
//
// 切り替えは再起動で行う。ボタンを押したまま再起動すると、IO9 が Low のまま
// リセットされて、書き込みモード (ダウンロードモード) に入ってしまうため、
// 必ず離してから再起動する。
// 長押しが成立した (1.5 秒たった) ときは、LED を水色にして合図する
// 設定を全て初期値に戻して、再起動する (BOOT ボタンを 20 秒押し続けたとき)
static void factoryReset()
{
    Serial.println(F("# BOOT held 20 s: factory reset"));

    // 全ての LED を赤で点滅 (250ms ごとに点灯と消灯)
    const uint32_t startMs = millis();
    bool on = false;
    while (millis() - startMs < FACTORY_RESET_BLINK_MS) {
        on = !on;
        displayFill(on ? CRGB(ERROR_VALUE, 0, 0) : CRGB::Black);
        delay(250);
    }
    displayFill(CRGB::Black);

    settingsFactoryReset();
    Serial.println(F("# all settings reset to default"));

    // ボタンを押したまま再起動すると、書き込みモードに入ってしまうので、離されるまで待つ
    while (g_bootButton.isDown()) delay(10);
    delay(50);                          // チャタリングが収まるまで
    bootRebootInto(false);
}

static void handleButton()
{
    switch (g_bootButton.poll()) {
        case BTN_LONG_REACHED:
            g_holdFeedback = true;              // 描画を止めて、水色のままにする
            Serial.println(F("# BOOT long press detected"));
            displayFill(CRGB(0, 50, 50));
            break;

        case BTN_VERY_LONG_REACHED:
            factoryReset();             // 戻らない (再起動する)
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

// 固定色モードの色。設定した明度に、フィルタの出力 x 反映量 (valMod) を足す。
// 気圧が上がると (+) 明るく、下がると (-) 暗くなる。結果は 0〜100 % に収める
// (明るさの最大値 brightness は掛けない)
//   明度 [0.1 %] = val x 10 + valMod x (フィルタの出力 -1000〜1000) / 100
static CRGB solidColor()
{
    const int32_t v = g_set.val * 10 + (g_set.valMod * g_solidLevel) / 100;
    return hsvToColorFine(g_set.hue, g_set.sat, v);     // 0〜1000 に収める
}

// 気圧の CSV を 1 行出す (LOG_EVERY_N ティックに 1 回)
//   millis,pressure_hPa,raw,hpf,temperature_C,altitude_m,led_value,battery_mV
// battery_mV は最後に測った電池電圧 (BATTERY_INTERVAL_MS ごとに測る)。毎行、その値を出す。
// hpf は、表示に使う変化量 (ドライブモードでは、小さくなるときにゆっくり戻した値)
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

// 変化量を、ちらつかないように保持する (ドライブモード)。絶対値が今の値以上になったら、すぐに
// その値にし (速い変化を遅らせない)、小さくなるときは、約 2^shift サンプルで、ゆっくり戻す。
// shift = 0 ならそのまま返す。保持しないモードの間も、値を追いかけておく
// (切り替えた直後に、古い値が出ないように)
static int32_t holdDelta(int32_t hpf, int32_t shift)
{
    const int32_t x = hpf * 256;            // 小数部を 8 ビット持つ
    if (shift <= 0 || abs(x) >= abs(g_smoothAcc)) {
        g_smoothAcc = x;
    } else {
        g_smoothAcc += (x - g_smoothAcc) >> shift;
    }
    return g_smoothAcc / 256;
}

// センサの FIFO にたまった値 (センサが 30Hz で測った値。たいてい 1 個、ときどき 0 個か 2 個) を、
// 古い順に 1 個ずつ HPF に通して記録する。LED1 の色などの表示は、いちばん新しい値で更新する。
// logEnabled が false (USB ホストが見えない) ときは、誰も読まないので、ログを出さない
static void sampleOnce(uint32_t nowMs, bool logEnabled)
{
    SensorSample ss[SENSOR_FIFO_DEPTH];
    uint32_t i2cUs = 0;
    SensorFifoStatus fst;
    const uint8_t count = sensorReadFifo(ss, SENSOR_FIFO_DEPTH, i2cUs, fst);

    g_perf.i2c(i2cUs);
    if (fst.status == SENSOR_I2C_ERROR) g_perf.i2cError();
    // サンプルが抜けたかもしれない: 記録の時刻を、次のサンプルで合わせ直す
    if (fst.overflow || fst.dropped) devlogMarkGap();
    if (count == 0) return;                     // まだ測っていない (LED はそのまま)

    const SenseParams p = modeSense(currentMode());
    int32_t shown = 0;
    for (uint8_t i = 0; i < count; i++) {
        const int32_t hpf = g_pressureHpf.update(ss[i].raw, p.hpfShift);
        // 表示に使う変化量。ドライブモードでは、乗り物の中の速いゆれでちらつかないように、
        // 小さくなるときだけ、ゆっくり戻す
        shown = holdDelta(hpf, p.holdShift);
        // 設定モードの表示ページのグラフへ (設定モードでなければ何もしない)
        // (いちばん新しい値は、下で表示の色と一緒に送る)
        if (i + 1 < count) {
            const CRGB c = deltaToColor(shown, p.deltaLimit, p.deltaRange);
            portalPushSample(nowMs, ss[i].raw, shown, c.r, c.g, c.b);
        }
        // 基板の中への記録 (記録中だけ)。変化量は、保持する処理の前の値 (毎サンプルの記録の CSV で、
        // 気圧から求め直す値と同じにするため)。age は、この値のあとに測った値の数 (測った時刻を求める)
        devlogPush(ss[i].raw, p.hpfShift, ss[i].tempC, batteryLastMilliVolts(), hpf, count - 1 - i);
    }
    const SensorSample &s = ss[count - 1];      // いちばん新しい値

    portalSetSensor(s.hPa, s.tempC);            // 設定ページの表示用
    CRGB color = deltaToColor(shown, p.deltaLimit, p.deltaRange);

    // 固定色モードでは、フィルタの出力を明度に反映した色にする (標準モードの感度。
    // modeSense の既定)。履歴は、標準モードと同じく、5 個の LED に流れる
    if (modeIsSolid(currentMode())) {
        g_solidLevel = deltaLevel(shown, p.deltaLimit, p.deltaRange);
        color = solidColor();
    }

    // 平滑化せずそのまま反映する (ドライブモードを除く)。ここが応答性の要です。
    displaySetHead(color);

    // 複数台の表示 (送信・ハブのとき)。固定色モードでは、表示している色を送る
    linkPush(nowMs, s, shown, color);

    // 設定モードの表示ページへ (設定モードでなければ何もしない)
    portalPushSample(nowMs, s.raw, shown, color.r, color.g, color.b);
    // 音 (Modulino Buzzer をつないでいるときだけ)。LED の色の段階が上がったときに鳴らす。
    // 固定色で、気圧の変化を明度に反映しない (valMod = 0) ときは、色が変わらないので鳴らさない
    int32_t level = deltaLevel(shown, p.deltaLimit, p.deltaRange);
    if (modeIsSolid(currentMode())) level = (g_set.valMod > 0) ? g_solidLevel : 0;
    soundTick(nowMs, level);

    if (logEnabled) logPressure(nowMs, s, shown, colorPeak(color));
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
    Serial.println(F("=== PressureMonitor " FIRMWARE_VERSION " ==="));
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
    // 通常モードでは、役割が送信・ハブのときだけ、無線を起動する (link.h)
    // 基板の中への記録 (devlog.h)。記録の途中で再起動したときは、続きから記録する
    devlogBegin(&g_pressureHpf);

    if (bootIsSettingsMode()) {
        portalBegin();
    } else {
        Serial.printf("# role: %s (name %s, group %ld)\n", linkRoleName((uint8_t)g_set.role),
                      settingsNodeName().c_str(), (long)g_set.group);
        linkBegin();
    }

    g_sensorReady = sensorBegin();
    soundBegin();                   // Qwiic の Modulino Buzzer を探す (I2C は sensorBegin で初期化済み)
    if (g_sensorReady) {
        const SenseParams p = modeSense(currentMode());
        Serial.printf("# mode %u (%s)\n", currentMode() + 1, modeName(currentMode()));
        Serial.printf("# tick %lu ms (%lu Hz) / HPF shift %ld / hold shift %ld"
                      " / limit %ld range %ld (1/64 Pa)\n",
                      (unsigned long)TICK_INTERVAL_MS,
                      1000UL / TICK_INTERVAL_MS,
                      (long)p.hpfShift, (long)p.holdShift,
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
    hubService(millis());           // ハブのときだけ動く (受信したデータをスマホへ)
    devlogService(millis());        // 基板の中への記録の操作 (表示ページから)
    linkService(millis());          // 送信の装置のときだけ動く (ハブのチャンネルの探索)

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
        // 記録の状態は、LED5 の点滅で知らせる。USB の検出窓 (USB_PROBE_INTERVAL_MS ごとに、
        // 眠らずに起きている USB_PROBE_WINDOW_MS) の中で点滅するので、ライトスリープの時間を減らさない
        //   記録中             : 赤で 1 回 (0〜REC_BLINK_MS)
        //   記録中 (無線あり)  : 前半を赤、後半を水色 (0〜REC_BLINK_RADIO_MS)
        //   いっぱいで止まった : 黄色で 2 回 (0〜REC_BLINK_MS と、2〜3 x REC_BLINK_MS)。
        //                        記録を消去するか、新しく記録を始めるまで続ける
        const uint32_t phase = now % USB_PROBE_INTERVAL_MS;
        bool blink = false;
        CRGB blinkColor;
        if (devlogRecording() && (bootIsSettingsMode() || linkRadioOn())) {
            // 無線あり (Wi-Fi・ESP-NOW): 前半を赤、後半を水色
            blink      = phase < REC_BLINK_RADIO_MS;
            blinkColor = phase < REC_BLINK_RADIO_MS / 2 ? REC_COLOR_RECORDING : REC_COLOR_RADIO;
        } else if (devlogRecording()) {
            blink      = phase < REC_BLINK_MS;
            blinkColor = REC_COLOR_RECORDING;
        } else if (devlogFull()) {
            blink      = phase < REC_BLINK_MS || (phase >= 2 * REC_BLINK_MS && phase < 3 * REC_BLINK_MS);
            blinkColor = REC_COLOR_FULL;
        }
        if (bootIsSettingsMode()) {
            displaySetStatus(true, blink ? blinkColor
                                         : (portalHasClient() ? CRGB(0, 60, 0) : CRGB(0, 40, 40)));
        } else {
            displaySetStatus(blink, blinkColor);
        }
        // 固定色モードには、明るさの最大値を掛けない (色の明度で決める)
        displayRenderTrail(modeIsSolid(currentMode()) ? 255 : (uint8_t)g_set.brightness);
    }
    g_perf.render(micros() - tC);

    // 電池電圧は 1 分に 1 回。描画のあとに行うので、LED の更新には影響しない
    if (now - g_lastBatMs >= BATTERY_INTERVAL_MS) {
        g_lastBatMs = now;
        batteryMeasure();
    }

    if (hostPresent) g_perf.report(now, g_log);
}
