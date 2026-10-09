#include "modes.h"

#include "config.h"
#include "settings.h"

// 標準: 細かい動き向け。1 Pa ≒ 8.5 cm
//   不感帯 50 ≒ 0.78 Pa ≒ 6.6 cm、白になるのは 400 ≒ 6.25 Pa ≒ 53 cm、
//   時定数は約 8.5 秒 (30Hz)
static const SenseParams SENSE_STANDARD = { 50, 400, 8, 1 };

// ドライブ: 車や電車での移動向け。白になるのは 15000 ≒ 234 Pa ≒ 20 m。
// 不感帯は標準と同じ 50、時定数は 2^9 サンプル ≒ 17 秒 (ゆっくりした変化まで拾う)
static const SenseParams SENSE_DRIVE = { 50, 15000, 9, 1 };

SenseParams modeSense(uint8_t mode)
{
    switch (mode) {
        case MODE_DRIVE:
            return SENSE_DRIVE;
        case MODE_CUSTOM: {
            // 設定ページの操作中 (リアルタイムの反映) は、値が 1 つずつ書き換わるので、
            // 途中の組み合わせでも壊れないように、ここで範囲を整える
            SenseParams p = { g_set.deltaLimit, g_set.deltaRange,
                              g_set.hpfShift, g_set.trailStepTicks };
            if (p.deltaLimit < 1) p.deltaLimit = 1;
            if (p.deltaRange < p.deltaLimit + (PALETTE_STEPS - 1)) {
                p.deltaRange = p.deltaLimit + (PALETTE_STEPS - 1);
            }
            if (p.hpfShift < 4)  p.hpfShift = 4;
            if (p.hpfShift > 12) p.hpfShift = 12;
            if (p.trailStepTicks < 1) p.trailStepTicks = 1;
            return p;
        }
        default:
            return SENSE_STANDARD;
    }
}

bool modeIsSolid(uint8_t mode)
{
    return mode == MODE_SOLID;
}

const char *modeName(uint8_t mode)
{
    switch (mode) {
        case MODE_STANDARD: return "標準";
        case MODE_DRIVE:    return "ドライブ";
        case MODE_CUSTOM:   return "カスタム";
        case MODE_SOLID:    return "固定色";
        default:            return "?";
    }
}
