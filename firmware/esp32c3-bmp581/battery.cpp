#include "battery.h"

#include <Arduino.h>

#include "config.h"

uint32_t batteryReadMilliVolts()
{
    analogReadMilliVolts(PIN_VBAT);          // 1 回目は捨てる

    uint32_t sum = 0;
    for (uint8_t i = 0; i < BATTERY_SAMPLES; i++) {
        sum += analogReadMilliVolts(PIN_VBAT);
    }
    return (sum / BATTERY_SAMPLES) * BATTERY_DIVIDER;
}

void batteryReport()
{
    uint32_t mV = batteryReadMilliVolts();
    Serial.printf("# battery %lu.%03lu V%s\n",
                  (unsigned long)(mV / 1000), (unsigned long)(mV % 1000),
                  (mV < BATTERY_PRESENT_MV) ? " (not installed)" : "");
}
