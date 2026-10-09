/*
 * display.h - LED (WS2812B x5) の表示
 *
 * どのモードで何を表示するかは、メインのスケッチ (esp32c3-bmp581.ino) が決めます。
 * このモジュールは、LED への描画と送信だけを受け持ちます。
 *   - 履歴の表示 (LED1 がライブ値、LED2..5 は過去の LED1 の色)
 *   - 単色の表示 (全 LED が同じ色)
 *   - モード番号の表示 (BOOT ボタンを押したとき、番号の数だけ青で点灯)
 */
#pragma once

#include <stdint.h>
#include "led_types.h"

// FastLED の初期化。全 LED を白で点灯する (電源が入ったことを、すぐに知らせるため)。
// brightness は、その白の明るさの最大値 (0〜255)
void displayBegin(uint8_t brightness);
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

// 30Hz で呼ぶ。モード番号の表示中は、どちらも、番号の表示を優先する。
void displayRenderTrail(uint8_t brightness);   // 履歴の表示。brightness = 明るさの最大値 (0〜255)
void displayRenderSolid(const CRGB &c);        // 全 LED を同じ色にする (明るさの設定は掛けない)

// モード番号の表示。number の数だけ、先頭の LED を、durationMs の間、青で点灯する
void displayShowModeNumber(uint8_t number, uint32_t durationMs);
bool displayModeNumberActive();                // 表示中か

// LED1 の色と履歴を消す。固定色モードで、気圧の履歴が残って、眠れなくなるのを防ぐ
void displayClearTrail();


// 状態表示 (最後の LED を、状態の色で上書きする)。設定モードで使う
void displaySetStatus(bool enabled, const CRGB &c);
