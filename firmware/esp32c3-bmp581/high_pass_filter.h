/*
 * high_pass_filter.h - 1 次ハイパスフィルタ (固定小数点)
 *
 *   y[n] = y[n-1] + (x[n] - x[n-1]) - y[n-1] / 2^shift
 *
 * ゆっくりしたドリフト (自己発熱など) を取り除き、素早い変化だけを取り出します。
 * 時定数は「サンプル数」で決まり、約 2^shift サンプルです。
 *   30Hz で shift=8 (256 サンプル) なら 8.5 秒
 * 大きいほど、ゆっくりした変化まで拾い、戻りが遅くなります。
 *
 * 状態を持つので、チャンネル (センサ) ごとにインスタンスを作ります。
 *   HighPassFilter pressureHpf;
 *   HighPassFilter humidityHpf;     // Qwiic に別のセンサを足したとき
 *
 * Arduino にも設定にも依存しない、純粋な計算です。
 */
#pragma once

#include <stdint.h>

class HighPassFilter {
public:
    // x を入れて、フィルタの出力を返す (単位は x と同じ)。
    // 初回は x で内部状態を初期化するので、起動時に過渡応答は出ない。
    // shift は呼び出しごとに指定できるので、設定の変更がすぐに反映される
    int32_t update(int32_t x, int32_t shift);

    // 内部状態を捨てる (次の update() が、初回として扱われる)
    void reset();

private:
    int64_t prevX_ = 0;     // 前回の入力 (内部のスケール)
    int64_t y_     = 0;     // フィルタの出力の状態 (内部のスケール)
    bool    init_  = false;
};
