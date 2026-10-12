#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_mac.h>

#include "config.h"
#include "link.h"

const AppSettings SETTINGS_DEFAULT = { 50, 400, 8, 1, 255, 0, 30, 100, 50, 0, 0, LINK_GROUP_DEFAULT, LINK_CHANNEL, "", "", "" };
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
    g_set.brightness     = clampI(g_set.brightness, 0, 255);
    g_set.mode           = clampI(g_set.mode, 0, 3);
    g_set.hue            = clampI(g_set.hue, 0, 359);
    g_set.sat            = clampI(g_set.sat, 0, 100);
    g_set.val            = clampI(g_set.val, 0, 100);
    g_set.valMod         = clampI(g_set.valMod, 0, 100);
    g_set.role           = clampI(g_set.role, 0, LINK_ROLE_COUNT - 1);
    g_set.group          = clampI(g_set.group, 1, 255);
    g_set.hubChannel     = clampI(g_set.hubChannel, 1, 13);
}

void settingsLoad()
{
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    g_set.deltaLimit     = p.getInt("limit",  SETTINGS_DEFAULT.deltaLimit);
    g_set.deltaRange     = p.getInt("range",  SETTINGS_DEFAULT.deltaRange);
    g_set.hpfShift       = p.getInt("hpf",    SETTINGS_DEFAULT.hpfShift);
    g_set.trailStepTicks = p.getInt("trail",  SETTINGS_DEFAULT.trailStepTicks);
    g_set.brightness     = p.getInt("bright", SETTINGS_DEFAULT.brightness);
    g_set.mode           = p.getInt("pmode",  SETTINGS_DEFAULT.mode);
    g_set.hue            = p.getInt("hue",    SETTINGS_DEFAULT.hue);
    g_set.sat            = p.getInt("sat",    SETTINGS_DEFAULT.sat);
    g_set.val            = p.getInt("val",    SETTINGS_DEFAULT.val);
    g_set.valMod         = p.getInt("vmod",   SETTINGS_DEFAULT.valMod);
    g_set.role           = p.getInt("role",   SETTINGS_DEFAULT.role);
    g_set.group          = p.getInt("group",  SETTINGS_DEFAULT.group);
    g_set.hubChannel     = p.getInt("hubch",  SETTINGS_DEFAULT.hubChannel);
    p.getString("nname", g_set.nodeName, sizeof(g_set.nodeName));
    p.getString("wssid", g_set.wifiSsid, sizeof(g_set.wifiSsid));
    p.getString("wpass", g_set.wifiPass, sizeof(g_set.wifiPass));
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
    p.putInt("bright", g_set.brightness);
    p.putInt("hue",   g_set.hue);
    p.putInt("sat",   g_set.sat);
    p.putInt("val",   g_set.val);
    p.putInt("vmod",  g_set.valMod);
    p.putInt("role",  g_set.role);
    p.putInt("group", g_set.group);
    p.putInt("hubch", g_set.hubChannel);
    p.putString("nname", g_set.nodeName);
    p.putString("wssid", g_set.wifiSsid);
    p.putString("wpass", g_set.wifiPass);
    p.end();
}

// 表示モードの切り替えのたびに、他の設定を巻き込まず、モードだけを書く
void settingsSaveMode()
{
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putInt("pmode", g_set.mode);
    p.end();
}

void settingsReset()
{
    // 表示モード、役割、グループ番号、チャンネル、装置の名前、Wi-Fi は、そのまま残す
    const AppSettings keep = g_set;
    g_set = SETTINGS_DEFAULT;
    g_set.mode = keep.mode;
    g_set.role = keep.role;
    g_set.group = keep.group;
    g_set.hubChannel = keep.hubChannel;
    memcpy(g_set.nodeName, keep.nodeName, sizeof(g_set.nodeName));
    memcpy(g_set.wifiSsid, keep.wifiSsid, sizeof(g_set.wifiSsid));
    memcpy(g_set.wifiPass, keep.wifiPass, sizeof(g_set.wifiPass));
    settingsSave();
}

String settingsDefaultNodeName()
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);     // ESP-NOW の送信元と同じ MAC アドレス
    char buf[12];
    snprintf(buf, sizeof(buf), "ps-%02x%02x%02x", mac[3], mac[4], mac[5]);
    return String(buf);
}

String settingsNodeName()
{
    return g_set.nodeName[0] ? String(g_set.nodeName) : settingsDefaultNodeName();
}

void settingsCopyName(char *dst, const char *src, size_t maxBytes)
{
    size_t n = 0;
    while (*src) {
        const uint8_t c = (uint8_t)*src;
        // 1 文字のバイト数 (UTF-8 の先頭のバイトで決まる)
        size_t len = (c < 0x80) ? 1 : (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        if (n + len > maxBytes) break;
        bool ok = (c >= 0x20 && c != 0x7F);              // 制御文字は除く
        for (size_t i = 1; i < len; i++) {
            if (((uint8_t)src[i] & 0xC0) != 0x80) { ok = false; len = i; break; }
        }
        if (ok) {
            memcpy(dst + n, src, len);
            n += len;
        }
        src += len;
    }
    dst[n] = '\0';
}

void settingsFactoryReset()
{
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.clear();
    p.end();
    g_set = SETTINGS_DEFAULT;
    s_brownoutCount = 0;
}

String settingsHubLastIp()
{
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    String ip = p.getString("hubip", "");
    p.end();
    return ip;
}

// 変わったときだけ書く (フラッシュの書き込みを減らす)
void settingsSaveHubLastIp(const String &ip)
{
    if (ip == settingsHubLastIp()) return;
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putString("hubip", ip);
    p.end();
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
