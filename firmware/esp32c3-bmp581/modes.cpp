#include "modes.h"

#include "config.h"
#include "settings.h"

// 標準: 細かい動き向け。1 Pa ≒ 8.5 cm
//   不感帯 50 ≒ 0.78 Pa ≒ 6.6 cm、白になるのは 753 ≒ 11.8 Pa ≒ 100 cm
//   (100 cm / 8.5 cm/Pa x 64 = 753)、
//   時定数は約 8.5 秒 (30Hz)
static const SenseParams SENSE_STANDARD = { 50, 753, 8, 1, 0 };

// ドライブ: 車や電車での移動向け。外出時の記録 (1 秒間隔、約 11.5 時間) から決めた値
//   不感帯 256 ≒ 4 Pa ≒ 34 cm : 建物の中で静止しているときは、ほぼ消灯する
//   白になるのは 4096 ≒ 64 Pa ≒ 5.4 m : 乗り物の中の変化の、大きい方の 1 割で白になる
//   時定数、履歴の間隔は、標準と同じ (約 8.5 秒、毎フレーム)。変化量の保持もしない。
//   (時定数を長くしたり、変化量をなめらかにしたりすると、トンネルに入った瞬間やうちわの風の
//   ような速い変化が、目でとらえにくくなった。乗り物の中の速いゆれも、そのまま表示する)
static const SenseParams SENSE_DRIVE = { 256, 4096, 8, 1, 0 };

SenseParams modeSense(uint8_t mode)
{
    switch (mode) {
        case MODE_DRIVE:
            return SENSE_DRIVE;
        case MODE_CUSTOM: {
            // 設定ページの操作中 (リアルタイムの反映) は、値が 1 つずつ書き換わるので、
            // 途中の組み合わせでも壊れないように、ここで範囲を整える
            SenseParams p = { g_set.deltaLimit, g_set.deltaRange,
                              g_set.hpfShift, g_set.trailStepTicks, 0 };
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
            // 標準。固定色モードで、気圧の変化を明度に反映するときも、この感度を使う
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
