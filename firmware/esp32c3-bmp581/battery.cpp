#include "battery.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <stdio.h>

#include "config.h"

static uint32_t s_lastMv = 0;
static uint32_t s_simMv  = 0;       // 試験用の値 (0 = 使わない)

// 電池の種類。再起動 (ソフトウェア、ディープスリープ) をまたいで残す
enum BatteryChem : uint8_t { CHEM_UNKNOWN = 0, CHEM_STANDARD = 1, CHEM_NIZN = 2 };
static const uint32_t CHEM_MAGIC = 0x43484D31UL;
RTC_NOINIT_ATTR static uint32_t s_chemMagic;
RTC_NOINIT_ATTR static uint8_t  s_chem;

// 判定に使う、1 分ごとの値 (新しいものほど後ろ)。下がる速さを求めるため、
// BATTERY_SLOPE_SPAN_MIN + BATTERY_MEDIAN_N 回分を持つ
static const uint8_t HIST_N = BATTERY_SLOPE_SPAN_MIN + BATTERY_MEDIAN_N;
static uint16_t s_hist[HIST_N];
static uint8_t  s_histCount = 0;

static BatteryLevel s_level   = BATTERY_LEVEL_OK;
static uint32_t     s_median  = 0;          // 最後に判定に使った中央値 [mV] (0 = まだ)
static int32_t      s_slope   = 0;          // 1 時間あたりの変化 [mV/h]
static bool         s_slopeOk = false;
static bool         s_heavy   = false;

uint32_t batteryReadMilliVolts()
{
    analogReadMilliVolts(PIN_VBAT);          // 1 回目は捨てる

    uint32_t sum = 0;
    for (uint8_t i = 0; i < BATTERY_SAMPLES; i++) {
        sum += analogReadMilliVolts(PIN_VBAT);
    }
    return (sum / BATTERY_SAMPLES) * BATTERY_DIVIDER;
}

bool batteryPresent(uint32_t mv)
{
    return mv >= BATTERY_PRESENT_MV;
}

// 電池の種類を、電圧から見分ける (まだ見分けていないときだけ)
static void detectChem(uint32_t mv)
{
    if (s_chem != CHEM_UNKNOWN || !batteryPresent(mv)) return;
    s_chem      = mv >= BATTERY_NIZN_DETECT_MV ? CHEM_NIZN : CHEM_STANDARD;
    s_chemMagic = CHEM_MAGIC;
    Serial.printf("# battery: %lu mV at power on -> %s\n", (unsigned long)mv,
                  s_chem == CHEM_NIZN ? "NiZn" : "NiMH/alkaline");
}

void batteryMeasure()
{
    s_lastMv = s_simMv ? s_simMv : batteryReadMilliVolts();
    detectChem(s_lastMv);
}

uint32_t batteryLastMilliVolts()
{
    return s_lastMv;
}

void batteryBegin(bool powerOn)
{
    if (powerOn || s_chemMagic != CHEM_MAGIC || s_chem > CHEM_NIZN) {
        s_chem      = CHEM_UNKNOWN;
        s_chemMagic = CHEM_MAGIC;
    }
}

uint32_t batteryLowMv(bool heavy)
{
    return (s_chem == CHEM_NIZN ? BATTERY_LOW_MV_NIZN : BATTERY_LOW_MV) + (heavy ? BATTERY_HEAVY_OFFSET_MV : 0);
}

uint32_t batteryEmptyMv(bool heavy)
{
    return (s_chem == CHEM_NIZN ? BATTERY_EMPTY_MV_NIZN : BATTERY_EMPTY_MV) + (heavy ? BATTERY_HEAVY_OFFSET_MV : 0);
}

static void pushHistory(uint32_t mv)
{
    if (s_histCount == HIST_N) {
        for (uint8_t i = 1; i < HIST_N; i++) s_hist[i - 1] = s_hist[i];
        s_histCount--;
    }
    s_hist[s_histCount++] = (uint16_t)min(mv, (uint32_t)65535);
}

// s_hist[from, from + n) の中央値
static uint32_t medianOf(uint8_t from, uint8_t n)
{
    uint16_t v[BATTERY_MEDIAN_N];
    if (n > BATTERY_MEDIAN_N) n = BATTERY_MEDIAN_N;
    for (uint8_t i = 0; i < n; i++) v[i] = s_hist[from + i];
    for (uint8_t i = 1; i < n; i++) {           // 挿入ソート (5 個)
        const uint16_t x = v[i];
        int8_t j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
    return (n % 2) ? v[n / 2] : ((uint32_t)v[n / 2 - 1] + v[n / 2]) / 2;
}

BatteryLevel batteryUpdate(bool heavy, bool skip)
{
    s_heavy = heavy;
    if (skip || !batteryPresent(s_lastMv)) return s_level;

    pushHistory(s_lastMv);
    if (s_histCount < BATTERY_MIN_READINGS) return s_level;

    const uint8_t n = min(s_histCount, BATTERY_MEDIAN_N);
    s_median = medianOf(s_histCount - n, n);

    // 下がる速さ: 今の中央値と、BATTERY_SLOPE_SPAN_MIN 分前の中央値の差
    s_slopeOk = s_histCount >= HIST_N;
    s_slope   = s_slopeOk ? ((int32_t)s_median - (int32_t)medianOf(0, BATTERY_MEDIAN_N)) * 60 / BATTERY_SLOPE_SPAN_MIN : 0;

    BatteryLevel lv = BATTERY_LEVEL_OK;
    const uint32_t low = batteryLowMv(heavy);
    if (s_median < batteryEmptyMv(heavy)) {
        lv = BATTERY_LEVEL_EMPTY;
    } else if (s_median < low || (s_slopeOk && s_median < low + BATTERY_SLOPE_GATE_MV && s_slope <= BATTERY_SLOPE_MV_PER_H)) {
        lv = BATTERY_LEVEL_LOW;
    }
    if (lv > s_level) {                         // 悪くなる方にだけ変わる
        s_level = lv;
        Serial.printf("# battery: %s (median %lu mV, %ld mV/h%s)\n", batteryLevelName(lv),
                      (unsigned long)s_median, (long)s_slope, heavy ? ", heavy load" : "");
    }
    return s_level;
}

BatteryLevel batteryLevel()
{
    return s_level;
}

const char *batteryLevelName(BatteryLevel lv)
{
    switch (lv) {
        case BATTERY_LEVEL_LOW:   return "low";
        case BATTERY_LEVEL_EMPTY: return "empty";
        default:                  return "ok";
    }
}

void batteryDescribe(char *buf, uint32_t size)
{
    char slope[24];
    if (s_slopeOk) snprintf(slope, sizeof(slope), "%ldmV/h", (long)s_slope);
    else           snprintf(slope, sizeof(slope), "n/a");
    snprintf(buf, size, "level=%s median=%lu slope=%s chem=%s low=%lu empty=%lu%s%s",
             batteryLevelName(s_level), (unsigned long)s_median, slope,
             s_chem == CHEM_NIZN ? "nizn" : (s_chem == CHEM_STANDARD ? "nimh_alkaline" : "unknown"),
             (unsigned long)batteryLowMv(s_heavy), (unsigned long)batteryEmptyMv(s_heavy),
             s_heavy ? " heavy" : "", s_simMv ? " simulated" : "");
}

void batterySimulate(uint32_t mv)
{
    s_simMv = mv;
    if (mv == 0) return;
    for (uint8_t i = 0; i < HIST_N; i++) s_hist[i] = (uint16_t)min(mv, (uint32_t)65535);
    s_histCount = HIST_N - 1;                   // 次の batteryUpdate で、いっぱいになる
    s_lastMv    = mv;
}

bool batterySimulating()
{
    return s_simMv != 0;
}
