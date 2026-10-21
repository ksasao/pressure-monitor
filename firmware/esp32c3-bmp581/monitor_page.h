/*
 * monitor_page.h - 設定モードで最初に開くページ (気圧の表示)
 *
 * スマホが設定モードの Wi-Fi に接続したときに、最初に表示するページです。
 * 気圧の絶対値と変化量 (HPF の出力) を大きく表示し、その下に、グラフを画面いっぱいに描きます。
 * スマホを横向きにすると、数値を 1 行にまとめて、グラフを広く表示します。
 * 設定は、右上の「設定」ボタンから開きます (/settings。settings_page.h)。
 * 記録は、基板の中に行い (devlog.h)、「CSV 保存」で /log.csv をダウンロードします。
 *
 * データは WebSocket (/ws) で受け取ります。基板は、約 100ms ごとに、まとめたサンプルを
 * バイナリのフレームで送ります (portal.cpp の portalPushSample)。
 * フレームの形式 (リトルエンディアン)
 *   0  'M'
 *   1  version   (3)
 *   2  count     サンプル数
 *   3  mode      表示モード (0〜3)
 *   4  tempCx100 温度 [0.01 ℃] (int16)
 *   6  recFlags  基板の中への記録 (devlog.h)。bit0 = 記録中、bit1 = いっぱいで止まった、
 *                bit2 = 記録の領域がない (記録できない)
 *   7  recIntv   記録の間隔 [秒]。0 = 毎サンプル (30Hz)、1、30 (その間の平均)
 *   8  recCount  記録したサンプル数 (uint32)
 *  12  recCap    記録できるサンプル数 (uint32)
 *  16  dlSent    CSV のダウンロードで、送ったバイト数 (uint32)
 *  20  dlTotal   CSV の大きさ [バイト] (uint32)。0 = まだダウンロードしていない
 *  24  サンプル x count (各 16 バイト)
 *        0  ms    時刻 (基板の millis)
 *        4  raw   気圧 (1/64 Pa)。hPa = raw / 6400
 *        8  hpf   HPF の出力 (1/64 Pa)
 *       12  r,g,b LED1 の色
 *       15  reserved
 */
#pragma once

extern const char MONITOR_PAGE_HTML[];
