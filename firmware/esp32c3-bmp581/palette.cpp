#include "palette.h"

#include "config.h"

// 上昇 (青系) / 下降 (赤橙系)。r, g, b
static const uint8_t PAL_RISE[PALETTE_STEPS][3] = {
    {  0,   0,   1}, {  0,  48, 128}, { 64, 192, 255}, {255, 255, 255}
};
static const uint8_t PAL_FALL[PALETTE_STEPS][3] = {
    {  1,   0,   0}, {128,  48,   0}, {255, 192,  40}, {255, 255, 255}
};

CRGB deltaToColor(int32_t delta, int32_t limit, int32_t range)
{
    int32_t a = (delta >= 0) ? delta : -delta;
    if (a < limit) return CRGB::Black;
    if (a > range) a = range;

    // range - limit >= PALETTE_STEPS - 1 は、呼び出し側が保証するので、width >= 1
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

CRGB hsvToColor(int32_t hueDeg, int32_t satPct, int32_t valPct)
{
    if (hueDeg < 0) hueDeg = 0;
    if (hueDeg > 359) hueDeg = 359;
    if (satPct < 0) satPct = 0;
    if (satPct > 100) satPct = 100;
    if (valPct < 0) valPct = 0;
    if (valPct > 100) valPct = 100;

    // 0〜255 に直す (四捨五入)
    const int32_t v = (valPct * 255 + 50) / 100;
    const int32_t s = (satPct * 255 + 50) / 100;

    const int32_t sector = hueDeg / 60;                 // 0..5
    const int32_t f      = ((hueDeg % 60) * 255) / 60;  // 区間の中の位置 0..255

    const int32_t p = (v * (255 - s)) / 255;
    const int32_t q = (v * (255 - (s * f) / 255)) / 255;
    const int32_t t = (v * (255 - (s * (255 - f)) / 255)) / 255;

    int32_t r, g, b;
    switch (sector) {
        case 0:  r = v; g = t; b = p; break;
        case 1:  r = q; g = v; b = p; break;
        case 2:  r = p; g = v; b = t; break;
        case 3:  r = p; g = q; b = v; break;
        case 4:  r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return CRGB((uint8_t)r, (uint8_t)g, (uint8_t)b);
}
