/*
 * settings.h - 設定値 (設定モードで変更・保存できる値)
 *
 * 値はフラッシュ (NVS) に保存され、電源を切っても残ります。
 */
#pragma once

#include <stdint.h>

// 感度に関する値の単位は 1/64 Pa。1 Pa ≒ 8.5 cm の高さ変化です。
// (標準モードとドライブモードの値は、modes.cpp にあります)
//
//   deltaLimit     [カスタム] この変化量未満は消灯。50 ≒ 0.78 Pa ≒ 6.6 cm
//   deltaRange     [カスタム] この変化量で白に達する。400 ≒ 6.25 Pa ≒ 53 cm (細かい動き向け)
//                  広い範囲 (エレベータ等) を見るなら 15000 (≒ 20 m)
//   hpfShift       [カスタム] HPF の時定数。約 2^hpfShift サンプル。30Hz で 8 なら 8.5 秒。
//                  大きいほどゆっくりした変化まで拾い、戻りが遅くなる
//   trailStepTicks [カスタム] 色を後ろへ送る間隔 [ティック]。1 = 毎フレーム。
//                  大きくすると LED2..5 に長い時間の履歴が流れる (LED1 は常にライブ値)
//   brightness     [標準・ドライブ・カスタム共通] LED の明るさの最大値 (0〜255)。
//                  255 = 抑えない。固定色モードには掛からない (固定色は hue/sat/val で決める)
//   mode           今の表示モード (modes.h)。BOOT ボタンで切り替えると保存される
//   hue, sat, val  [固定色] 色相 (0〜359 度)、彩度 (0〜100 %)、明度 (0〜100 %)
//
// 感度は、ログの hpf 列を見て調整してください。
// 静止時の hpf の振れ幅より deltaLimit が小さいと、暗い色がちらつきます
struct AppSettings {
    int32_t deltaLimit;
    int32_t deltaRange;
    int32_t hpfShift;
    int32_t trailStepTicks;
    int32_t brightness;
    int32_t mode;
    int32_t hue;
    int32_t sat;
    int32_t val;
};

extern AppSettings        g_set;              // 現在の設定
extern const AppSettings  SETTINGS_DEFAULT;   // 既定値

void settingsLoad();      // 保存された設定を読む (なければ既定値)
void settingsSave();      // 現在の設定を保存する (mode を除く)
void settingsSaveMode();  // 今の表示モードだけを保存する
void settingsReset();     // 既定値に戻して保存する (表示モードは変えない)
void settingsClamp();     // 範囲外の値を直す

// ブラウンアウト (電源電圧の落ち込みによるリセット) の累計。保存される
uint32_t settingsBrownoutCount();
void     settingsAddBrownout();
