/*
 * modes.h - 表示モード
 *
 * BOOT ボタンの短押しで切り替える 4 種類のモード。
 *
 *   0 標準       細かい動き (数十 cm 〜 数 m) 向け。感度は固定
 *   1 ドライブ   車や電車での移動 (大きな高さの変化) 向け。感度は固定
 *   2 カスタム   感度を設定モード (Wi-Fi) で変えられる
 *   3 固定色     全ての LED が、設定した色 (HSV) で点灯する
 *
 * 標準・ドライブ・カスタムは、気圧の変化を LED1 の色にして、LED2〜5 に履歴を流す。
 * 3 つの違いは、感度の値 (SenseParams) だけです。
 */
#pragma once

#include <stdint.h>
#include "led_types.h"

enum DisplayMode : uint8_t {
    MODE_STANDARD = 0,
    MODE_DRIVE    = 1,
    MODE_CUSTOM   = 2,
    MODE_SOLID    = 3,
    MODE_COUNT    = 4,
};

// 気圧の変化を色にするときの感度 (単位は 1/64 Pa。settings.h を参照)
struct SenseParams {
    int32_t deltaLimit;
    int32_t deltaRange;
    int32_t hpfShift;
    int32_t trailStepTicks;
};

// モードの感度。標準とドライブは固定値、カスタムは g_set の値。
// 固定色モードでは使われない (標準と同じ値を返す)
SenseParams modeSense(uint8_t mode);

// 固定色モードか
bool modeIsSolid(uint8_t mode);

// モードの名前 (ログと設定ページ用)
const char *modeName(uint8_t mode);
