/*
 * palette.h - 変化量を色にする (純粋な計算)
 *
 *   変化量の絶対値が deltaLimit 未満なら黒 (不感帯)。
 *   deltaRange を上限に、4 点のパレットを線形補間して色にする。
 *     変化量が正 (気圧上昇): 黒青 -> 青 -> 水色 -> 白
 *     変化量が負 (気圧下降): 黒赤 -> 橙 -> 黄橙 -> 白
 *
 * deltaLimit と deltaRange は引数で受け取ります (モードごとに違う。modes.h)。
 */
#pragma once

#include <stdint.h>
#include "led_types.h"

// 変化量 -> 色。delta > 0 は青系、delta < 0 は赤橙系。
// limit 未満は黒、range 以上は白。range >= limit + (PALETTE_STEPS - 1) であること
CRGB deltaToColor(int32_t delta, int32_t limit, int32_t range);

// 色相 (0〜359 度)、彩度 (0〜100 %)、明度 (0〜100 %) -> 色。標準的な HSV (FastLED の rainbow ではない)
CRGB hsvToColor(int32_t hueDeg, int32_t satPct, int32_t valPct);

// hsvToColor の明度を、0.1 % 単位 (0〜1000) で指定する版。明度を滑らかに変えるときに使う
CRGB hsvToColorFine(int32_t hueDeg, int32_t satPct, int32_t valPermille);

// 変化量を -1000〜1000 にする (符号は delta と同じ)。|delta| が limit 未満なら 0、
// range 以上なら ±1000、その間は比例。deltaToColor と同じ範囲で、色ではなく、量がほしいときに使う
int32_t deltaLevel(int32_t delta, int32_t limit, int32_t range);

// 色の RGB のうち、最大の値 (ログの led_value に使う)
uint8_t colorPeak(const CRGB &c);
