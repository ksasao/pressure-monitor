#include "boot.h"

#include <Arduino.h>
#include <esp_system.h>           // esp_reset_reason()

// 設定モードへ入るための、再起動をまたぐ目印。
// RTC メモリに置くと、ソフトウェア再起動 (ESP.restart) では値が残ります。
// 電源投入直後は不定値なので、リセット要因が SW かどうかも一緒に見ます
RTC_NOINIT_ATTR uint32_t g_bootMagic;
static const uint32_t BOOT_MAGIC_SETTINGS = 0x5345544EUL;

static bool               s_settingsMode = false;
static esp_reset_reason_t s_resetReason  = ESP_RST_UNKNOWN;

void bootDetect()
{
    s_resetReason  = esp_reset_reason();
    s_settingsMode = (s_resetReason == ESP_RST_SW && g_bootMagic == BOOT_MAGIC_SETTINGS);
    g_bootMagic    = 0;           // 1 回限り。次の再起動では通常モードに戻る
}

bool bootIsSettingsMode()
{
    return s_settingsMode;
}

bool bootWasBrownout()
{
    return s_resetReason == ESP_RST_BROWNOUT;
}

const char *bootResetReasonName()
{
    switch (s_resetReason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        default:                return "OTHER";
    }
}

void bootRebootInto(bool settings)
{
    g_bootMagic = settings ? BOOT_MAGIC_SETTINGS : 0;
    Serial.printf("# rebooting into %s mode\n",
                  settings ? "settings (Wi-Fi)" : "normal");
    delay(100);
    ESP.restart();
}
