/*
 * boot.h - 起動の理由と、通常モード / 設定モードの切り替え
 *
 * 設定モード (Wi-Fi) と通常モードの切り替えは、再起動で行います。
 * 再起動をまたぐ目印は RTC メモリに置くので、ソフトウェア再起動
 * (ESP.restart) のときだけ、設定モードに入ります。
 */
#pragma once

// setup() の最初に 1 回呼ぶ。リセットの理由を調べ、設定モードかどうかを決める。
// 設定モードへの指示は 1 回限りで、次の再起動では通常モードに戻る
void bootDetect();

bool        bootIsSettingsMode();     // 設定モード (Wi-Fi) で起動したか
bool        bootWasBrownout();        // 前回、電源電圧の落ち込みでリセットされたか
const char *bootResetReasonName();    // "POWERON", "SW", "BROWNOUT" など

// 設定モード (true) と通常モード (false) を、再起動で切り替える。
// ボタンを押したまま呼ばないこと (IO9 が Low のままリセットされ、
// 書き込みモード (ダウンロードモード) に入ってしまう)
void bootRebootInto(bool settings);
