

#include "ButtonHandler.h"

void ButtonHandler::begin() {
    pinMode(BTN_A, INPUT_PULLUP);
    pinMode(BTN_B, INPUT_PULLUP);
}

ButtonEvent ButtonHandler::getEvent() {
    unsigned long now = millis();
    bool aPressed = (digitalRead(BTN_A) == LOW);
    bool bPressed = (digitalRead(BTN_B) == LOW);

    if (aPressed && !_aWasPressed) {
        _aPressTime = now;
        _aWasPressed = true;
    }

    if (bPressed && !_bWasPressed) {
        _bPressTime = now;
        _bWasPressed = true;
    }

    if (aPressed && bPressed) {
        if (!_comboActive) {
            _comboActive = true;
            _comboPressTime = min(_aPressTime, _bPressTime);
        }
        return BTN_NONE;
    }

    if (_comboActive) {
        if (!aPressed && !bPressed) {
            const unsigned long held = now - _comboPressTime;
            _comboActive = false;
            _aWasPressed = false;
            _bWasPressed = false;
            if (held >= DEBOUNCE_MS) {
                return BTN_AB_SHORT;
            }
        }
        return BTN_NONE;
    }

    if (!aPressed && _aWasPressed) {
        const unsigned long held = now - _aPressTime;
        _aWasPressed = false;
        if (held >= DEBOUNCE_MS) {
            return (held >= LONG_PRESS_MS) ? BTN_A_LONG : BTN_A_SHORT;
        }
    }

    if (!bPressed && _bWasPressed) {
        const unsigned long held = now - _bPressTime;
        _bWasPressed = false;
        if (held >= DEBOUNCE_MS) {
            return (held >= LONG_PRESS_MS) ? BTN_B_LONG : BTN_B_SHORT;
        }
    }

    return BTN_NONE;
}



