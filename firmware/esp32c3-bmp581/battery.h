/*
 * battery.h - 電池電圧 (AA(+)x2)
 *
 * R9/R10 (各 1MΩ) で 1/2 に分圧して、IO1 (ADC1_CH1) で読みます。
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
