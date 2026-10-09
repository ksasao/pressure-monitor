#include "battery.h"

#include <Arduino.h>

#include "config.h"

static uint32_t s_lastMv = 0;

uint32_t batteryReadMilliVolts()
{
    analogReadMilliVolts(PIN_VBAT);          // 1 回目は捨てる

    uint32_t sum = 0;
    for (uint8_t i = 0; i < BATTERY_SAMPLES; i++) {
        sum += analogReadMilliVolts(PIN_VBAT);
    }
    return (sum / BATTERY_SAMPLES) * BATTERY_DIVIDER;
}

void batteryMeasure()
{
    s_lastMv = batteryReadMilliVolts();
}

uint32_t batteryLastMilliVolts()
{
    return s_lastMv;
}
