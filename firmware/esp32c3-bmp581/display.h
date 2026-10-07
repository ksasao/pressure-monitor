/*
 * display.h - LED (WS2812B x5) の表示
 *
 * 表示モード (BOOT ボタンの短押しで、0 -> 1 -> 2 -> 3 -> 0 と切り替わる)
 *   0 = 気圧の変化 (LED1 がライブ値、LED2..5 は過去の LED1 の色)
 *   1 = 気圧の絶対値を 5 段階のバーで表示
 *   2 = 消灯 (省電力)
 *   3 = 全点灯 (診断用)。全 LED を白で点灯し、毎ティック LED へ送る。
 *       電源の電圧の落ち込みや、LED のデータの乱れを調べるためのモード
 */
#pragma once

#include <stdint.h>
#include "led_types.h"

static const uint8_t DISPLAY_MODE_COUNT = 4;     // 表示モードの数

void displayBegin();                       // FastLED の初期化。全消灯する
void displayApplyPower();                  // settings の maxMilliamps (電流の上限) を反映する
void displaySelfTest();                    // 起動時の自己診断。全 LED を順に R -> G -> B
void displayError();                       // センサ異常の表示 (呼ぶたびに赤の点滅が反転する)
void displayFill(const CRGB &c);           // 全 LED を単色にする (長押しの合図など)

// LED へのデータの送信が終わるまで待つ (最大 5ms)。
// FastLED の show() は非同期で、送信の完了を待たずに戻る。
// 送信の途中でライトスリープに入ると、RMT のクロックが止まって、データが壊れ、
// LED が意図しない色 (緑など) で光る。眠る前に、必ず呼ぶこと
void displayWait();

// LED が全て消えていて、履歴にも色が残っていないか。ライトスリープに入ってよい条件の 1 つ。
// 点灯している間は眠らない (起きたままなら、LED への送信は乱れない)
bool displayIsDark();

// ライトスリープから復帰したことを知らせる。復帰してから、最初に LED へ送るときだけ、
// 同じフレームを 2 回送る (復帰直後の最初の送信が化けることがあるため)。
// 全消灯で内容が変わらないあいだは、LED へ送らないので、化けようがない
void displayNoteWake();

void displaySetHead(const CRGB &c);        // LED1 の色 (ライブ値) を更新する
void displayTrailShift();                  // 履歴を 1 段後ろに送り、先頭に LED1 の色を入れる

void displayRender(uint8_t mode, float hPa);   // 30Hz で呼ぶ。モードに応じて描画する
void displayModeChanged(uint8_t mode);         // 表示モードを切り替えた直後に呼ぶ

// 状態表示 (最後の LED を、状態の色で上書きする)。設定モードで使う
void displaySetStatus(bool enabled, const CRGB &c);
