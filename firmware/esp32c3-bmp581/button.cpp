#include "button.h"

#include <Arduino.h>

Button::Button(uint8_t pin, uint32_t longPressMs)
    : pin_(pin), longPressMs_(longPressMs)
{
}

void Button::begin()
{
    pinMode(pin_, INPUT_PULLUP);
}

ButtonEvent Button::poll()
{
    uint32_t    now   = millis();
    bool        down  = (digitalRead(pin_) == LOW);
    ButtonEvent event = BTN_NONE;

    if (down && !prevDown_) {
        downAt_    = now;
        longFired_ = false;
    }

    if (down && !longFired_ && (now - downAt_) >= longPressMs_) {
        longFired_ = true;
        event      = BTN_LONG_REACHED;
    }

    if (!down && prevDown_) {
        if (longFired_) {
            event = BTN_LONG_RELEASED;
        } else if ((now - downAt_) > 40) {      // チャタリング対策
            event = BTN_SHORT;
        }
    }

    prevDown_ = down;
    return event;
}
