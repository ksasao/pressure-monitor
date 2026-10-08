#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>

#include "config.h"

const AppSettings SETTINGS_DEFAULT = { 50, 400, 8, 1, 0, 255 };
AppSettings g_set = SETTINGS_DEFAULT;

static const char *NVS_NAMESPACE = "pmon";
static uint32_t    s_brownoutCount = 0;

static int32_t clampI(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

// 範囲外の値を直す。deltaRange は、パレットの区間が 1 以上になるように
// deltaLimit + (PALETTE_STEPS - 1) 以上にする
void settingsClamp()
{
    g_set.deltaLimit     = clampI(g_set.deltaLimit, 1, 5000);
    g_set.deltaRange     = clampI(g_set.deltaRange,
                                  g_set.deltaLimit + (PALETTE_STEPS - 1), 100000);
    g_set.hpfShift       = clampI(g_set.hpfShift, 4, 12);
    g_set.trailStepTicks = clampI(g_set.trailStepTicks, 1, 60);
    g_set.defaultMode    = clampI(g_set.defaultMode, 0, 2);
    g_set.brightness     = clampI(g_set.brightness, 0, 255);
}

void settingsLoad()
{
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    g_set.deltaLimit     = p.getInt("limit",  SETTINGS_DEFAULT.deltaLimit);
    g_set.deltaRange     = p.getInt("range",  SETTINGS_DEFAULT.deltaRange);
    g_set.hpfShift       = p.getInt("hpf",    SETTINGS_DEFAULT.hpfShift);
    g_set.trailStepTicks = p.getInt("trail",  SETTINGS_DEFAULT.trailStepTicks);
    g_set.defaultMode    = p.getInt("mode",   SETTINGS_DEFAULT.defaultMode);
    g_set.brightness     = p.getInt("bright", SETTINGS_DEFAULT.brightness);
    s_brownoutCount      = p.getUInt("bo", 0);
    p.end();
    settingsClamp();
}

void settingsSave()
{
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putInt("limit", g_set.deltaLimit);
    p.putInt("range", g_set.deltaRange);
    p.putInt("hpf",   g_set.hpfShift);
    p.putInt("trail", g_set.trailStepTicks);
    p.putInt("mode",  g_set.defaultMode);
    p.putInt("bright", g_set.brightness);
    p.end();
}

void settingsReset()
{
    g_set = SETTINGS_DEFAULT;
    settingsSave();
}

uint32_t settingsBrownoutCount()
{
    return s_brownoutCount;
}

void settingsAddBrownout()
{
    s_brownoutCount++;
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putUInt("bo", s_brownoutCount);
    p.end();
}
