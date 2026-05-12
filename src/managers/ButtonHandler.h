


#pragma once
#include <Arduino.h>
#include "config.h"

enum ButtonEvent {
    BTN_NONE,
    BTN_A_SHORT,
    BTN_A_LONG,
    BTN_B_SHORT,
    BTN_B_LONG,
    BTN_AB_SHORT
};

class ButtonHandler {
public:
    void begin();
    ButtonEvent getEvent();

private:
    unsigned long _aPressTime = 0;
    unsigned long _bPressTime = 0;
    unsigned long _comboPressTime = 0;
    bool _aWasPressed = false;
    bool _bWasPressed = false;
    bool _comboActive = false;
    static const unsigned long LONG_PRESS_MS = 800;
    static const unsigned long DEBOUNCE_MS = 50;
};




