/*
 * battery.h - 電池電圧 (AA(+)x2) と、電池切れの判定
 *
 * R9/R10 (各 1MΩ) で 1/2 に分圧して、IO1 (ADC1_CH1) で読みます。
 *
 * 電池切れの判定 (config.h の BATTERY_*)
 *   1 分ごとに測った電圧の、最近 BATTERY_MEDIAN_N 回の中央値で判定します。
 *   - 電池が少ない (BATTERY_LEVEL_LOW): LED5 の点滅で知らせる (esp32c3-bmp581.ino)
 *   - 電池切れ (BATTERY_LEVEL_EMPTY): 記録を閉じて、ディープスリープに入る
 *   一度「電池が少ない」になったら、電源を入れ直すまで戻りません (電圧は、負荷が減ると少し戻るため)。
 *   USB から給電されている間は、判定しません (電池に電流が流れず、正しく判定できないため)。
 *   電池の種類は、電源を入れたときの電圧で見分けます (ニッケル亜鉛か、それ以外か)。
 *   ソフトウェアの再起動 (設定モードとの切り替え) や、ディープスリープのあとも、見分けた結果を残します。
 */
#pragma once

#include <stdint.h>

// 電池電圧 [mV] を返す。分圧 (1/2) を戻した値。
// 分圧の出力抵抗が約 500kΩ と高いので、C12 (100nF) に頼って複数回読んで平均する。
// ADC の誤差は数 % あるので、気になる場合はテスターの実測と突き合わせてください
uint32_t batteryReadMilliVolts();

// 電池電圧を測って、結果を保存する (batteryLastMilliVolts() で読める)。
// 起動時に 1 回と、その後は BATTERY_INTERVAL_MS ごとに呼ぶ
void batteryMeasure();

// 最後に測った電池電圧 [mV]。シリアルの CSV に出す値。測る前は 0
uint32_t batteryLastMilliVolts();

enum BatteryLevel : uint8_t {
    BATTERY_LEVEL_OK    = 0,
    BATTERY_LEVEL_LOW   = 1,    // 電池が少ない (知らせる)
    BATTERY_LEVEL_EMPTY = 2,    // 電池切れ (止める)
};

// setup() で、最初の batteryMeasure() より前に 1 回呼ぶ。powerOn は、電源を入れた (電池を入れた) ときか。
// そのときだけ、電池の種類を見分け直す
void batteryBegin(bool powerOn);

// batteryMeasure() のあとに呼ぶ (1 分ごと)。heavy: 今、電流の大きい使い方をしている。
// skip: USB から給電されている (判定しない。測った値も、判定に使わない)
BatteryLevel batteryUpdate(bool heavy, bool skip);

BatteryLevel batteryLevel();
const char  *batteryLevelName(BatteryLevel lv);

// 電池が入っているか (SW1 が ON か)
bool batteryPresent(uint32_t mv);

// 判定の電圧 [mV] (電池の種類と、電流の大きい使い方かどうかで変わる)
uint32_t batteryLowMv(bool heavy);
uint32_t batteryEmptyMv(bool heavy);

// status コマンドの 1 行 ("level=ok median=2650 ...")
void batteryDescribe(char *buf, uint32_t size);

// 試験用: 測った値の代わりに mv を使う (0 = やめる)。判定に使う過去の値も、全て mv にする
void batterySimulate(uint32_t mv);
bool batterySimulating();
