/*
 * settings_page.h - 設定ページ (HTML) と、状況の確認用の JSON
 *
 * 文字列を組み立てるだけで、通信や状態の保存はしません。
 */
#pragma once

#include <Arduino.h>

// /status が返す、機器の状況
struct DeviceStatus {
    uint32_t    vbatNowMv;      // 電池電圧 [mV]
    uint32_t    vbatMinMv;      // 設定モードに入ってからの最低値 [mV]。0 = 未測定
    float       hPa;
    float       tempC;
    uint8_t     stations;       // 接続中のスマホの台数
    uint32_t    freeHeap;       // 空きメモリ [B]
    const char *resetReason;    // 前回の再起動の理由
    uint32_t    brownouts;      // ブラウンアウトによる再起動の累計
};

// 設定ページ。現在の設定を埋め込む。saved = true なら「保存しました」を出す。
// 電池電圧などの状況は、ページ内のスクリプトが /status を 1 秒ごとに取得して更新する
String buildSettingsPage(bool saved);

// /status の JSON
String buildStatusJson(const DeviceStatus &s);
