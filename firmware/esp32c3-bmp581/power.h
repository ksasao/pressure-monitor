/*
 * power.h - 省電力 (USB が接続されていないときの、ライトスリープ)
 *
 * ティックの待ち時間 (33ms 周期のうち、処理をしていない約 28ms) を、
 * delay() で CPU を止める代わりに、ライトスリープで過ごします。
 * ライトスリープ中の消費電流は、CPU を止めるだけの場合より、ずっと小さくなります。
 *
 * 眠るのは、次の条件がすべて揃っているときだけです (config.h)。
 *   - 通常モード (設定モードは Wi-Fi を使うので、眠らない)
 *   - 起動から LIGHT_SLEEP_BOOT_DELAY_MS 以上たっている
 *   - USB ホスト (PC) が、USB_UNPLUGGED_HOLD_MS 以上、見えていない
 *   - 検出窓の時間 (USB_PROBE_WINDOW_MS) ではない
 *
 * LED との関係
 *   FastLED の show() は非同期で、送信の完了を待たずに戻る。送信の途中で
 *   ライトスリープに入ると、RMT のクロックが止まって、LED のデータが壊れ、
 *   緑などの意図しない色で光る。そのため、眠る直前に displayWait() で、
 *   送信の完了を待っている。
 *
 * USB ホストの検出
 *   Serial.isPlugged() を使う。これは USB のフレーム信号 (SOF) を、FreeRTOS の
 *   ティックのフックで監視していて、ライトスリープ中は動かない。
 *   眠った直後の値は古い可能性があるので、「眠っていない時間」だけ判定する。
 *
 * USB の接続について
 *   - 電池駆動 (USB なし) で眠っているときに、USB ケーブルを PC へ挿すと、
 *     眠っている間は、PC から見えません。数秒以内の検出窓で、起きているときに、
 *     接続が始まります。うまく認識されないときは、RESET ボタンを押してください
 *     (リセット後の 15 秒は、眠りません)。
 *   - USB ホストが見えないとき (電池駆動、または USB は電源だけ) は、誰も読まない
 *     ログ (CSV、性能計測) の出力を止めて、CPU の時間を節約します。
 */
#pragma once

#include <stdint.h>

// loop() から毎回呼ぶ。USB ホストが見えているかを調べる
void powerService(uint32_t nowMs);

// 最近 (USB_UNPLUGGED_HOLD_MS 以内)、USB ホストが見えたか。
// false のときは、ログを出す意味がない
bool powerHostPresent(uint32_t nowMs);

// 次のティックまでの待ち。眠ってよければ、ライトスリープで過ごす。
// 眠れない (条件を満たさない、または失敗) ときは、delay(1) で待つ。
// remainingMs: 次のティックまでの時間 [ms]
void powerIdle(uint32_t nowMs, uint32_t remainingMs);
