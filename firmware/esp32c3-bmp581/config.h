/*
 * config.h - 調整できる定数と、回路図に合わせたピン割り当て
 *
 * 多くのモジュールから参照される値を、ここにまとめています。
 * 設定モード (Wi-Fi) のページから変更できる値 (感度など) は、
 * settings.cpp の SETTINGS_DEFAULT が既定値です。
 */
#pragma once

#include <stdint.h>

// ---------------------------------------------------------------- pin map
// 回路図 P1 より
#define PIN_NEO 10                // U2 IO10 -> R8 -> LED1 DIN
                                  // FastLED はピンをテンプレート引数に取るため
                                  // コンパイル時定数である必要があります

static const uint8_t PIN_SDA  = 4;    // U2 IO4  -> U1 SDI
static const uint8_t PIN_SCL  = 5;    // U2 IO5  -> U1 SCK
static const uint8_t PIN_BOOT = 9;    // U2 IO9  (タクトスイッチ, active low)
static const uint8_t PIN_VBAT = 1;    // U2 IO1  (ADC1_CH1) 電池電圧の分圧

static const uint8_t NUM_LEDS = 5;

// BMP581 の I2C アドレス: SDO=GND -> 0x46 / SDO=VDDIO -> 0x47
static const uint8_t ADDR_SDO_LOW  = 0x46;
static const uint8_t ADDR_SDO_HIGH = 0x47;

// ------------------------------------------------------------------ rate
// センサ読み取り / ログ / 描画を全部この周期で回します
static const uint32_t TICK_INTERVAL_MS = 33;        // ≒ 30 Hz

// ログの間引き。1 = 毎ティック出力 (30 行/秒)
static const uint8_t LOG_EVERY_N = 1;

// 1 秒ごとに「どこが遅いか」を '#' 付きの 1 行で出力する
static const bool ENABLE_PERF_LOG = true;

// ----------------------------------------------------------------- power
// CPU クロック [MHz]。80 にしています。
// 40MHz では、ライトスリープから復帰した直後に、LED へのデータの補充が間に合わず、
// 点灯し始めに、最後の LED (LED4、LED5) が、一瞬緑になりました。
// (待つ時間や、送信の回数を増やしても直らず、80MHz にすると直ることを、実機で確認済み)
// 眠っている間の電流は、CPU クロックに依存しないので、80MHz でも、全消灯のときの
// 平均電流は、ほぼ変わりません。LED が点灯している間 (眠らない) は、約 4mA 増えます。
// 80MHz 未満では APB クロックも CPU に連動して下がります。
static const uint32_t CPU_MHZ = 80;

// 設定モード (Wi-Fi) の CPU クロック。Wi-Fi は 80MHz 以上が必要です
static const uint32_t SETTINGS_CPU_MHZ = 80;

// ------------------------------------------------------------ light sleep
// USB ホスト (PC) が接続されていないときだけ、ティックの待ち時間に
// ライトスリープに入ります (power.h)。
// USB の通信 (Serial) は、ライトスリープ中は使えないため、
// USB が接続されているあいだは、眠りません。
static const bool LIGHT_SLEEP_ENABLED = true;

// 起動してから、この時間は眠らない。リセット直後に USB を接続して、
// 書き込みやシリアルモニタを使えるようにするため
static const uint32_t LIGHT_SLEEP_BOOT_DELAY_MS = 15000;

// USB ホストが、この時間以上、見えなかったら、眠ってよい (接続の直後は、
// 検出が不安定なため)
static const uint32_t USB_UNPLUGGED_HOLD_MS = 3000;

// 眠っている間も、この間隔で、検出窓の時間だけ、眠らずに起きている。
// ケーブルを挿されたとき、起きている間でないと、ホストが接続を始められない。
// 窓を長くすると確実ですが、省電力の効果が減ります
static const uint32_t USB_PROBE_INTERVAL_MS = 5000;
static const uint32_t USB_PROBE_WINDOW_MS   = 400;

// 次のティックまで、この時間以上あるときだけ眠る。
// 復帰の遅れを見込んで、その分だけ早めに起きる
static const uint32_t LIGHT_SLEEP_MIN_MS         = 3;
static const uint32_t LIGHT_SLEEP_WAKE_MARGIN_US = 1000;

// LED の乱れの対策 (点灯し始めに、最後の LED が、一瞬緑になる症状)。
// 原因は、CPU が遅い (40MHz) と、ライトスリープから復帰した直後に、LED への
// データの補充が間に合わず、データが化けることです (CPU_MHZ の説明を参照)。
// 次の対策を入れています (display.h)。
//   - CPU クロックを 80MHz にする
//   - 全消灯で、前回と同じ内容なら、LED へ送らない (送信がなければ、化けない)
//   - LED が点灯している間は、眠らない
//   - 眠った後の、最初の送信だけ、念のため、同じフレームを 2 回送る
//
// LED_SKIP_UNCHANGED : 内容が前回と同じなら、LED へ送らない
// LED_REFRESH_MS     : 内容が変わらなくても、この間隔で 1 回は送り直す (LED の表示が
//                      乱れたときに、自動で戻すため)。0 = 送り直さない
static const bool     LED_SKIP_UNCHANGED = true;
static const uint32_t LED_REFRESH_MS     = 30000;

// --------------------------------------------------------------- battery
// 電池電圧 (AA(+)x2) は、R9/R10 (各 1MΩ) で 1/2 に分圧して IO1 に入っています。
// 1 分に 1 回、'#' で始まる 1 行でシリアルに出力します
static const uint32_t BATTERY_INTERVAL_MS = 60000;   // 1 分
static const uint32_t BATTERY_DIVIDER     = 2;       // R9 = R10 = 1MΩ
static const uint8_t  BATTERY_SAMPLES     = 16;      // 平均する回数
static const uint32_t BATTERY_PRESENT_MV  = 1000;    // これ未満は電池なしとみなす

// ------------------------------------------------------------ BOOT button
// BOOT ボタンの長押しと判定する時間
static const uint32_t LONG_PRESS_MS = 1500;

// ------------------------------------------------------ settings mode (Wi-Fi)
// スマホで読み取る QR コードに入れる文字列 (QR は別途作成してください):
//   Wi-Fi 接続 (パスワードなし) : WIFI:T:nopass;S:pressure-monitor;;
//   Wi-Fi 接続 (パスワードあり) : WIFI:T:WPA;S:pressure-monitor;P:<パスワード>;;
//   設定ページ                  : http://192.168.4.1/
// IP アドレスは、アクセスポイントの既定値 (192.168.4.1) をそのまま使います
//
// パスワードが空 (または 8 文字未満) のときは、パスワードなし (オープン) で起動します。
// iPhone での検証では、パスワードなしで、接続したまま設定ページが開きました。
// パスワードを付ける場合は、同じように動くか確認してください
static const char *AP_SSID          = "pressure-monitor";
static const char *AP_PASSWORD      = "";
static const uint8_t AP_CHANNEL     = 1;
static const uint8_t AP_MAX_CLIENTS = 2;

// 送信出力。ブラウンアウト (電源電圧の落ち込みによるリセット) が出る場合は、
// WIFI_POWER_8_5dBm などに下げると、送信時のピーク電流が減ります。
// (型を持つ定数にすると config.h が WiFi の型に依存するため、マクロにしています)
#define AP_TX_POWER WIFI_POWER_19_5dBm

// キャプティブポータルの応答方式 (スマホが Wi-Fi に接続したときの接続確認への応答)
//   false : 設定ページへ転送する (302)
//   true  : 設定ページの HTML を、そのまま 200 で返す
// どちらも、iPhone で接続したまま設定ページが開くことを確認済みです。
// 注意: iOS の接続確認に "Success" と返す方式は、iPhone が 2 回目の確認
// (netcts.cdn-apple.com) で弾かれて、接続できませんでした
static const bool PORTAL_DIRECT_HTML = false;

// 設定ページの操作 (表示・保存) がないまま、この時間がたったら通常モードに戻る
static const uint32_t SETTINGS_IDLE_TIMEOUT_MS = 10UL * 60UL * 1000UL;

// ---------------------------------------------------------------- display
// パレットの点数。(deltaRange - deltaLimit) をこの数 - 1 で割って区間にします
static const int32_t PALETTE_STEPS = 4;

// モード 1 (気圧バー) の表示範囲 [hPa] と、満杯の LED の明るさ
static const float   BAR_MIN_HPA   = 995.0f;
static const float   BAR_MAX_HPA   = 1025.0f;
static const uint8_t BAR_MAX_VALUE = 120;

// 起動時の自己診断とエラー表示の明るさ
static const uint8_t SELFTEST_VALUE = 120;
static const uint8_t ERROR_VALUE    = 120;

// 消費電力の上限。FastLED が自動で輝度を抑えてくれます。
// 白は 5 個全点灯で電流が最大になるので、電池駆動ではこの制限が効きます。
// FastLED の電流モデルは 5V の WS2812B 基準なので、3.3V 動作では
// 実際より多めに見積もられます (= 安全側に効きます)
// 電流の上限 (mA) は settings の maxMilliamps で、設定モードから変更できます
static const uint8_t PWR_VOLTS = 3;

// ----------------------------------------------------------------- sensor
// 標高計算に使う基準気圧 [hPa]
static const float SEA_LEVEL_HPA = 1013.25f;
