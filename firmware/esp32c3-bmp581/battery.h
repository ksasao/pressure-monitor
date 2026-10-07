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

// シリアルに 1 行出力する。例: "# battery 2.874 V"
// 電池が入っていない (USB 給電中など) ときは "(not installed)" が付く
void batteryReport();
