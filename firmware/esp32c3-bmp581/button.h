/*
 * button.h - ボタン (INPUT_PULLUP、押すと Low)
 *
 * ボタンを離したときに、結果が決まります。
 *   短押し                     : BTN_SHORT
 *   長押し (longPressMs 以上)   : 押している間に BTN_LONG_REACHED (1 回)、
 *                                離したときに BTN_LONG_RELEASED
 *
 *   Button boot(PIN_BOOT, LONG_PRESS_MS);
 *   boot.begin();                     // setup() で
 *   ButtonEvent e = boot.poll();      // loop() で毎回
 */
#pragma once

#include <stdint.h>

enum ButtonEvent {
    BTN_NONE,
    BTN_SHORT,            // 短押しして、離した
    BTN_LONG_REACHED,     // 長押しの時間に達した (まだ押している)
    BTN_LONG_RELEASED     // 長押しして、離した
};

class Button {
public:
    Button(uint8_t pin, uint32_t longPressMs);

    void        begin();    // ピンの設定
    ButtonEvent poll();     // ループから毎回呼ぶ

private:
    uint8_t  pin_;
    uint32_t longPressMs_;

    bool     prevDown_  = false;
    uint32_t downAt_    = 0;
    bool     longFired_ = false;
};
