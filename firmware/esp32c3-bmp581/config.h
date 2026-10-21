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

// ---------------------------------------------------------------- boot
// 電源が入ったときの、白の全点灯を見せる最短の時間 [ms]。
// この後に、自己診断 (赤 -> 緑 -> 青の順に 1 個ずつ点灯) が続く
static const uint32_t BOOT_WHITE_MS = 500;

// ----------------------------------------------------------------- power
// CPU クロック [MHz]。80 にしています。
// 40MHz では、ライトスリープから復帰した直後に、LED へのデータの補充が間に合わず、
// 点灯し始めに、最後の LED (LED4、LED5) が、一瞬緑になりました。
// (待つ時間や、送信の回数を増やしても直らず、80MHz にすると直ることを、実機で確認済み)
// 眠っている間の電流は、CPU クロックに依存しないので、80MHz でも、全消灯のときの
// 平均電流は、ほぼ変わりません。
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

// LED が点灯している間も、ティックの待ち時間に眠る。
// WS2812B は、データを受け取ったあとは、マイコンが眠っても色を保つので、
// 起きてセンサを読み、LED へ送ったら、次のティックまで眠れる。
// 風の強い日など、LED が点灯している時間が長いときの電池の持ちが、大きく延びる
// (起きている時間が 33ms から数 ms になる)。
// false にすると、以前と同じく、点灯している間は眠らない (LED の乱れが出たときの確認用)
static const bool LIGHT_SLEEP_WHILE_LIT = true;

// LED の乱れの対策 (点灯し始めに、最後の LED が、一瞬緑になる症状)。
// 原因は、CPU が遅い (40MHz) と、ライトスリープから復帰した直後に、LED への
// データの補充が間に合わず、データが化けることです (CPU_MHZ の説明を参照)。
// 次の対策を入れています (display.h)。
//   - CPU クロックを 80MHz にする
//   - 全消灯で、前回と同じ内容なら、LED へ送らない (送信がなければ、化けない)
//   - 眠った後の、最初の送信だけ、念のため、同じフレームを 2 回送る (LED_RESEND_AFTER_WAKE)
//   - LIGHT_SLEEP_WHILE_LIT を false にすると、点灯している間は眠らない (以前の動作)
//
// LED_RESEND_AFTER_WAKE : 眠った後の最初の送信を、2 回送る。点灯中も眠る場合は、毎フレームが
//                      「眠った後」になるので、起きている時間が約 1.5ms 延びる。
//                      80MHz で乱れが出ないことを確かめたら、false にすると、さらに省電力になる
// LED_SKIP_UNCHANGED : 内容が前回と同じなら、LED へ送らない
// LED_REFRESH_MS     : 内容が変わらなくても、この間隔で 1 回は送り直す (LED の表示が
//                      乱れたときに、自動で戻すため)。0 = 送り直さない
static const bool     LED_RESEND_AFTER_WAKE = true;
static const bool     LED_SKIP_UNCHANGED = true;
static const uint32_t LED_REFRESH_MS     = 30000;

// --------------------------------------------------------------- battery
// 電池電圧 (AA(+)x2) は、R9/R10 (各 1MΩ) で 1/2 に分圧して IO1 に入っています。
// 1 分に 1 回、'#' で始まる 1 行でシリアルに出力します
static const uint32_t BATTERY_INTERVAL_MS = 60000;   // 1 分
static const uint32_t BATTERY_DIVIDER     = 2;       // R9 = R10 = 1MΩ
static const uint8_t  BATTERY_SAMPLES     = 16;      // 平均する回数
static const uint32_t BATTERY_PRESENT_MV  = 1000;    // これ未満は電池なしとみなす

// ------------------------------------------------------------- recording
// 基板の中への記録 (devlog.h) の状態を、LED5 の点滅で知らせる。
//   記録中             : USB_PROBE_INTERVAL_MS (5 秒) ごとに、赤で REC_BLINK_MS だけ 1 回点灯
//   記録中 (無線あり)  : 同じ間隔で、前半を赤、後半を水色で、REC_BLINK_RADIO_MS だけ点灯。
//                        設定モード (Wi-Fi) や、役割が送信・ハブ (ESP-NOW) のとき。
//                        電池の消費が大きい状態であることを知らせる
//   いっぱいで止まった : 同じ間隔で、黄色で 2 回点灯 (記録を消去するか、新しく始めるまで)
// 点灯するのは、USB の検出窓 (眠らずに起きている USB_PROBE_WINDOW_MS) の中なので、
// ライトスリープの時間を減らさない (2 回の点滅は 3 x REC_BLINK_MS で、窓に収まる)。
// 明るさは、LED の明るさの最大値の設定 (brightness) が掛かる
static const uint32_t REC_BLINK_MS = 100;
static_assert(3 * REC_BLINK_MS <= USB_PROBE_WINDOW_MS, "blink must fit in the USB probe window");
// 無線を使っている間は、ライトスリープに入らないので、検出窓に合わせなくてよい。
// 赤と水色を見分けられるように、それぞれ REC_BLINK_MS (LED の更新 3 回分) にする
static const uint32_t REC_BLINK_RADIO_MS = 2 * REC_BLINK_MS;
#define REC_COLOR_RECORDING CRGB(80, 0, 0)     // 赤
#define REC_COLOR_RADIO     CRGB(0, 40, 80)    // 水色 (無線ありの記録中の後半)
#define REC_COLOR_FULL      CRGB(80, 45, 0)    // 黄色 (WS2812B は緑が強いので、緑を控えめに)

// ------------------------------------------------------------ BOOT button
// BOOT ボタンの長押しと判定する時間
static const uint32_t LONG_PRESS_MS = 1500;

// BOOT ボタンを、この時間押し続けると、全ての LED を赤で点滅したあと (FACTORY_RESET_BLINK_MS)、
// 設定を全て初期値に戻して、再起動する (ボタンを離してから)
static const uint32_t FACTORY_RESET_PRESS_MS = 20000;
static const uint32_t FACTORY_RESET_BLINK_MS = 1000;

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

// ------------------------------------------------- link (複数台の表示, ESP-NOW)
// 役割 (設定モードで選ぶ。link.h):
//   単体 : 今までどおり。無線はオフ
//   送信 : 気圧のデータを、ESP-NOW のブロードキャストで送る
//   ハブ : 送信の装置のデータを受け取り、スマホのブラウザに表示する。自分の気圧も、同じように
//          表示する。設定ページで指定した Wi-Fi (ルーター) に接続するか、指定がなければ、
//          自分でアクセスポイント (HUB_SSID、http://192.168.4.1/) になる (hub.h)
// 送信とハブは、無線を使うため、ライトスリープに入りません (電池の持ちが短くなります)。
//
// ハブが、自分でアクセスポイントになるときのチャンネルの初期値 (1〜13)。設定ページで変えられます
// (settings の hubChannel)。Wi-Fi のルーターに接続するときは、ルーターのチャンネルになります。
// 送信の装置は、ハブのチャンネルを自動で探すので、ハブだけ変えれば済みます。
// 混雑した場所では、空いているチャンネル (重ならない 1、6、11 のどれか) に変えてください
static const uint8_t LINK_CHANNEL = 1;

// グループ番号の初期値 (1〜255)。設定ページで変えられます (settings の group)。
// 同じグループ番号の送信の装置とハブだけが、やり取りします。近くで、本機の別のセットが
// 動いているときは、セットごとに別の番号にします
static const int32_t LINK_GROUP_DEFAULT = 39;

// チャンネルの自動探索 (link.h)
//   LINK_BEACON_INTERVAL_MS : ハブがビーコンを送る間隔
//   LINK_SCAN_DWELL_MS      : 送信の装置が、1 つのチャンネルで、ビーコンを待つ時間
//                             (ビーコンの間隔より長くする。13 チャンネルで最大 約 4 秒)
//   LINK_BEACON_LOST_MS     : ビーコンがこの時間途切れたら、探し直す
static const uint32_t LINK_BEACON_INTERVAL_MS = 250;
static const uint32_t LINK_SCAN_DWELL_MS      = 300;
static const uint32_t LINK_BEACON_LOST_MS     = 3000;

// ハブが Wi-Fi (ルーター) に接続を試みる時間。つながらなければ、自分でアクセスポイントになる
static const uint32_t HUB_STA_TIMEOUT_MS = 20000;

// ハブがルーターに接続している間も、自分のアクセスポイント (HUB_SSID) を立ち上げたままにする。
// スマホをこれにつないで http://192.168.4.1/ を開けば、PC がなくても、ルーターから割り当てられた
// IP アドレスを確認できます (表示ページの上部)。データも、そのまま見られます。
// アクセスポイントのチャンネルは、ルーターのチャンネルに合わせて変わります
static const bool HUB_KEEP_AP_WITH_STA = true;

// ハブの mDNS の名前。http://pressure-hub.local/ で開ける (Wi-Fi のルーターに接続したとき)
static const char *HUB_MDNS_NAME = "pressure-hub";

// 1 回の送信にまとめるサンプル数。3 なら、約 100ms ごとに 1 回送る (30Hz のデータ)
static const uint8_t LINK_SAMPLES_PER_PACKET = 3;

// 送信の装置の送信出力。ブラウンアウトが出る場合は、WIFI_POWER_8_5dBm などに下げます
#define LINK_TX_POWER WIFI_POWER_19_5dBm

// ハブのアクセスポイント。パスワードは付けません
static const char   *HUB_SSID        = "pressure-hub";
static const uint8_t HUB_MAX_CLIENTS = 4;            // 同時に表示できるスマホの台数

// ハブがスマホへデータを送る間隔 [ms]
static const uint32_t HUB_PUSH_INTERVAL_MS = 100;

// ---------------------------------------------------------------- display
// パレットの点数。(deltaRange - deltaLimit) をこの数 - 1 で割って区間にします
static const int32_t PALETTE_STEPS = 4;

// 起動時の自己診断とエラー表示の明るさ
static const uint8_t SELFTEST_VALUE = 120;
static const uint8_t ERROR_VALUE    = 120;

// LED の明るさの最大値 (0〜255) は、settings の brightness で、設定モードから変更できます。
// 電流の上限による制限 (FastLED の setMaxPowerInVoltsAndMilliamps) は、使っていません。
// 電流を実測してから、必要なら入れます

// ----------------------------------------------------------------- sensor
// 標高計算に使う基準気圧 [hPa]
static const float SEA_LEVEL_HPA = 1013.25f;
