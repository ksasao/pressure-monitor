/*
 * led_types.h - FastLED の型 (CRGB など) を使うためのヘッダ
 *
 * FastLED.h を直接 include する代わりに、このヘッダを使います。
 * (FastLED の警告の抑制を、1 か所にまとめるため)
 */
#pragma once

#ifndef FASTLED_INTERNAL
#define FASTLED_INTERNAL          // 「FastLED version ...」の警告を抑制
#endif
#include <FastLED.h>
