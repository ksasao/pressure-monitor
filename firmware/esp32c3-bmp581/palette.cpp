#include "palette.h"

#include "config.h"
#include "settings.h"

// 上昇 (青系) / 下降 (赤橙系)。r, g, b
static const uint8_t PAL_RISE[PALETTE_STEPS][3] = {
    {  0,   0,   1}, {  0,  48, 128}, { 64, 192, 255}, {255, 255, 255}
};
static const uint8_t PAL_FALL[PALETTE_STEPS][3] = {
    {  1,   0,   0}, {128,  48,   0}, {255, 192,  40}, {255, 255, 255}
};

CRGB deltaToColor(int32_t delta)
{
    const int32_t limit = g_set.deltaLimit;
    const int32_t range = g_set.deltaRange;

    int32_t a = (delta >= 0) ? delta : -delta;
    if (a < limit) return CRGB::Black;
    if (a > range) a = range;

    // range - limit >= PALETTE_STEPS - 1 は settingsClamp() が保証するので、width >= 1
    const int32_t width = (range - limit) / (PALETTE_STEPS - 1);
    int32_t index = (a - limit) / width;
    int32_t mid   = (a - limit) - index * width;

    // 上端ちょうど (a == range) は最後の点を使う。
    // (index + 1 がパレットの配列の範囲外にならないようにするため)
    if (index >= PALETTE_STEPS - 1) {
        index = PALETTE_STEPS - 2;
        mid   = width;
    }

    const uint8_t (*pal)[3] = (delta > 0) ? PAL_RISE : PAL_FALL;

    CRGB c;
    c.r = (uint8_t)((pal[index][0] * (width - mid) + pal[index + 1][0] * mid) / width);
    c.g = (uint8_t)((pal[index][1] * (width - mid) + pal[index + 1][1] * mid) / width);
    c.b = (uint8_t)((pal[index][2] * (width - mid) + pal[index + 1][2] * mid) / width);
    return c;
}

uint8_t colorPeak(const CRGB &c)
{
    uint8_t v = c.r;
    if (c.g > v) v = c.g;
    if (c.b > v) v = c.b;
    return v;
}
