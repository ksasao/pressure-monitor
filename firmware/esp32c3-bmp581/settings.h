/*
 * settings.h - 設定値 (設定モードで変更・保存できる値)
 *
 * 値はフラッシュ (NVS) に保存され、電源を切っても残ります。
 */
#pragma once

#include <stdint.h>

// 単位は 1/64 Pa。1 Pa ≒ 8.5 cm の高さ変化です。
//
//   deltaLimit     この変化量未満は消灯。50 ≒ 0.78 Pa ≒ 6.6 cm
//   deltaRange     この変化量で白に達する。400 ≒ 6.25 Pa ≒ 53 cm (細かい動き向け)
//                  広い範囲 (エレベータ等) を見るなら 15000 (≒ 20 m)
//   hpfShift       HPF の時定数。約 2^hpfShift サンプル。30Hz で 8 なら 8.5 秒。
//                  大きいほどゆっくりした変化まで拾い、戻りが遅くなる
//   trailStepTicks 色を後ろへ送る間隔 [ティック]。1 = 毎フレーム。
//                  大きくすると LED2..5 に長い時間の履歴が流れる (LED1 は常にライブ値)
//   defaultMode    起動時の表示モード (0 = 気圧変化, 1 = バー, 2 = 消灯)
//   brightness     LED の明るさの最大値 (0〜255)。全ての色に掛かる。255 = 抑えない
//
// 感度は、ログの hpf 列を見て調整してください。
// 静止時の hpf の振れ幅より deltaLimit が小さいと、暗い色がちらつきます
struct AppSettings {
    int32_t deltaLimit;
    int32_t deltaRange;
    int32_t hpfShift;
    int32_t trailStepTicks;
    int32_t defaultMode;
    int32_t brightness;
};

extern AppSettings        g_set;              // 現在の設定
extern const AppSettings  SETTINGS_DEFAULT;   // 既定値

void settingsLoad();      // 保存された設定を読む (なければ既定値)
void settingsSave();      // 現在の設定を保存する
void settingsReset();     // 既定値に戻して保存する
void settingsClamp();     // 範囲外の値を直す

// ブラウンアウト (電源電圧の落ち込みによるリセット) の累計。保存される
uint32_t settingsBrownoutCount();
void     settingsAddBrownout();
