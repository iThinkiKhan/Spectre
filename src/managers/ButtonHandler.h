


#pragma once
#include <Arduino.h>
#include "config.h"

enum ButtonEvent {
    BTN_NONE,
    BTN_A_SHORT,
    BTN_A_LONG,
    BTN_B_SHORT,
    BTN_B_LONG,
    BTN_AB_SHORT,
    BTN_AB_LONG
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
    bool _comboLongFired = false;
    // Sourced from config.h. These used to be hardcoded here while config.h
    // carried an identical, unreferenced copy — so editing the config did
    // nothing. Keep them pointed at the config macros.
    static const unsigned long LONG_PRESS_MS = BUTTON_LONG_PRESS_MS;
    // Global sleep chord: hold both buttons this long to fire BTN_AB_LONG.
    // Longer than the single-button long-press so a two-hand grab that lands
    // slightly staggered still reads as a deliberate sleep gesture.
    static const unsigned long AB_LONG_PRESS_MS = BUTTON_AB_LONG_PRESS_MS;
    static const unsigned long DEBOUNCE_MS = BUTTON_DEBOUNCE_MS;
};




