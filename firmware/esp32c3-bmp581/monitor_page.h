/*
 * monitor_page.h - 設定モードで最初に開くページ (気圧の表示)
 *
 * スマホが設定モードの Wi-Fi に接続したときに、最初に表示するページです。
 * 気圧の絶対値と変化量 (HPF の出力) を大きく表示し、その下に、グラフを画面いっぱいに描きます。
 * スマホを横向きにすると、数値を 1 行にまとめて、グラフを広く表示します。
 * 設定は、右上の「設定」ボタンから開きます (/settings。settings_page.h)。
 *
 * データは WebSocket (/ws) で受け取ります。基板は、約 100ms ごとに、まとめたサンプルを
 * バイナリのフレームで送ります (portal.cpp の portalPushSample)。
 * フレームの形式 (リトルエンディアン)
 *   0  'M'
 *   1  version   (1)
 *   2  count     サンプル数
 *   3  mode      表示モード (0〜3)
 *   4  tempCx100 温度 [0.01 ℃] (int16)
 *   6  reserved  0 (uint16)
 *   8  サンプル x count (各 16 バイト)
 *        0  ms    時刻 (基板の millis)
 *        4  raw   気圧 (1/64 Pa)。hPa = raw / 6400
 *        8  hpf   HPF の出力 (1/64 Pa)
 *       12  r,g,b LED1 の色
 *       15  reserved
 */
#pragma once

extern const char MONITOR_PAGE_HTML[];
