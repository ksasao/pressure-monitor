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
 *   - 眠っている間は、USB の回路も止まるので、PC から見えません。
 *   - V1.1 の基板は、TPS2116 の ST (IO3) で、USB から給電されているかが分かります。
 *     USB から給電されている間は眠らず、眠っている間に USB の給電が始まったら、
 *     GPIO の復帰ですぐに起きます。そのため、電池で動いているときに USB ケーブルを
 *     PC へ挿しても、すぐに認識されます。
 *   - V1.0 の基板 (IO3 が未接続。起動時に見分ける) は、眠っている間も、
 *     USB_PROBE_INTERVAL_MS ごとに USB_PROBE_WINDOW_MS だけ起きていて、その間に
 *     接続が始まります。うまく認識されないときは、RESET ボタンを押してください
 *     (リセット後の 15 秒は、眠りません)。
 *   - USB ホストが見えないとき (電池駆動、または USB は電源だけ) は、誰も読まない
 *     ログ (CSV、性能計測) の出力を止めて、CPU の時間を節約します。
 */
#pragma once

#include <stdint.h>

// setup() から 1 回呼ぶ。IO3 (TPS2116 の ST) がつながっているかを見分け、
// つながっていれば、USB の給電が始まったときに、ライトスリープから起きるようにする
void powerBegin();

// USB から給電されているか (IO3 が High)。V1.0 の基板では、常に false
bool powerUsbPowered();

// loop() から毎回呼ぶ。USB ホストが見えているかを調べる
void powerService(uint32_t nowMs);

// 最近 (USB_UNPLUGGED_HOLD_MS 以内)、USB ホストが見えたか。
// false のときは、ログを出す意味がない
bool powerHostPresent(uint32_t nowMs);

// 次のティックまでの待ち。眠ってよければ、ライトスリープで過ごす。
// 眠れない (条件を満たさない、または失敗) ときは、delay(1) で待つ。
// remainingMs: 次のティックまでの時間 [ms]
void powerIdle(uint32_t nowMs, uint32_t remainingMs);

// 以後、ライトスリープに入らない (無線を使う、送信・ハブの役割のとき)
void powerKeepAwake();
