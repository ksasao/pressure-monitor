/*
 * palette.h - 変化量を色にする (純粋な計算)
 *
 *   変化量の絶対値が deltaLimit 未満なら黒 (不感帯)。
 *   deltaRange を上限に、4 点のパレットを線形補間して色にする。
 *     変化量が正 (気圧上昇): 黒青 -> 青 -> 水色 -> 白
 *     変化量が負 (気圧下降): 黒赤 -> 橙 -> 黄橙 -> 白
 *
 * deltaLimit と deltaRange は、settings.h の g_set を参照します。
 */
#pragma once

#include <stdint.h>
#include "led_types.h"

// 変化量 -> 色。delta > 0 は青系、delta < 0 は赤橙系
CRGB deltaToColor(int32_t delta);

// 色の RGB のうち、最大の値 (ログの led_value に使う)
uint8_t colorPeak(const CRGB &c);
