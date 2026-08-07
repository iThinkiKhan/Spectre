


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
            _comboLongFired = false;
            _comboPressTime = min(_aPressTime, _bPressTime);
        }
        // Fire the sleep chord once while still held so the gesture gives
        // immediate feedback instead of waiting for release.
        if (!_comboLongFired &&
            (now - _comboPressTime) >= AB_LONG_PRESS_MS) {
            _comboLongFired = true;
            return BTN_AB_LONG;
        }
        return BTN_NONE;
    }

    if (_comboActive) {
        if (!aPressed && !bPressed) {
            const unsigned long held = now - _comboPressTime;
            const bool longFired = _comboLongFired;
            _comboActive = false;
            _comboLongFired = false;
            _aWasPressed = false;
            _bWasPressed = false;
            // Suppress the trailing short-combo when the long chord already
            // fired, so one hold can't emit both AB_LONG and AB_SHORT.
            if (!longFired && held >= DEBOUNCE_MS) {
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




