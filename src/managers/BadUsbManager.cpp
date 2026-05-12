


#include "BadUsbManager.h"

#include <ctype.h>

#include "../core/DebugLog.h"
#include "../core/SpectreState.h"
#include "../managers/StorageManager.h"

#if defined(ARDUINO_USB_MODE) && (ARDUINO_USB_MODE == 0)
  #define SPECTRE_BADUSB_TINYUSB_MODE 1
#else
  #define SPECTRE_BADUSB_TINYUSB_MODE 0
#endif

#if SPECTRE_BADUSB_TINYUSB_MODE
  #include "USB.h"
  #include "USBHID.h"
  #include "USBHIDKeyboard.h"
#endif

namespace {

#if SPECTRE_BADUSB_TINYUSB_MODE
USBHIDKeyboard g_badUsbKeyboard;
bool g_badUsbUsbStarted = false;

bool badUsbHidSupported() {
    return true;
}

bool badUsbHidBegin() {
    if (g_badUsbUsbStarted) {
        return true;
    }

    g_badUsbKeyboard.begin(KeyboardLayout_en_US);
    g_badUsbKeyboard.setShiftKeyReports(true);
    g_badUsbUsbStarted = USB.begin();
    return g_badUsbUsbStarted;
}

bool badUsbHostReady() {
    return g_badUsbUsbStarted && static_cast<bool>(USB);
}

void badUsbReleaseAll() {
    if (badUsbHostReady()) {
        g_badUsbKeyboard.releaseAll();
    }
}

bool badUsbPress(uint8_t key) {
    return badUsbHostReady() && (g_badUsbKeyboard.press(key) == 1);
}

bool badUsbWrite(uint8_t key) {
    return badUsbHostReady() && (g_badUsbKeyboard.write(key) == 1);
}
#else
bool badUsbHidSupported() {
    return false;
}

bool badUsbHidBegin() {
    return false;
}

bool badUsbHostReady() {
    return false;
}

void badUsbReleaseAll() {}

bool badUsbPress(uint8_t) {
    return false;
}

bool badUsbWrite(uint8_t) {
    return false;
}
#endif

bool parseUint16Strict(const String& text, uint16_t& outValue) {
    String trimmed = text;
    trimmed.trim();
    if (trimmed.isEmpty()) {
        return false;
    }

    for (size_t i = 0; i < trimmed.length(); ++i) {
        if (!isdigit(static_cast<unsigned char>(trimmed.charAt(i)))) {
            return false;
        }
    }

    const unsigned long value = strtoul(trimmed.c_str(), nullptr, 10);
    if (value > 0xFFFFUL) {
        return false;
    }

    outValue = static_cast<uint16_t>(value);
    return true;
}

String uppercaseToken(const String& token) {
    String upper = token;
    upper.trim();
    upper.toUpperCase();
    return upper;
}

}

bool BadUsbManager::begin() {
    STORAGE.ensureBadUsbVault();
    _ready = badUsbHidSupported() && badUsbHidBegin();
    refreshScripts();

    if (_ready) {
        _setStatus("IDLE");
    } else {
        _setStatus("UNAVAIL", true, "USB HID OFF");
    }

    return _ready;
}

int BadUsbManager::refreshScripts() {
    int stateSelected = 0;
    STATE_READ_BEGIN();
    stateSelected = g_state.badUsbListSelected;
    STATE_READ_END();

    _scriptCount = STORAGE.loadBadUsbScriptIndex(_scripts, MAX_SCRIPTS);

    if (_scriptCount <= 0) {
        _selected = -1;
    } else if (stateSelected >= 0 && stateSelected < _scriptCount) {
        _selected = stateSelected;
    } else if (_selected < 0 || _selected >= _scriptCount) {
        _selected = 0;
    }

    _updateSelectedState(_scriptCount <= 0);
    DLOG_INFO("BADUSB", "Loaded %d scripts", _scriptCount);
    return _scriptCount;
}

bool BadUsbManager::selectScript(int index) {
    if (index < 0 || index >= _scriptCount) {
        _setStatus("ERROR", true, "BAD INDEX");
        return false;
    }

    if (_state == BADUSB_COUNTDOWN || _state == BADUSB_RUNNING) {
        _setStatus("ERROR", true, "BUSY");
        return false;
    }

    _selected = index;
    _armedScript = "";
    _resetExecution();
    _state = BADUSB_IDLE;
    _updateSelectedState();
    _setStatus("IDLE");
    DLOG_INFO("BADUSB", "Selected script idx=%d name=%s",
              _selected,
              _scripts[_selected].name);
    return true;
}

bool BadUsbManager::armSelected(int index) {
    if (!_ready) {
        _setStatus("ERROR", true, "USB HID OFF");
        return false;
    }

    if (index < 0 || index >= _scriptCount) {
        _setStatus("ERROR", true, "BAD INDEX");
        return false;
    }

    if (_state == BADUSB_COUNTDOWN || _state == BADUSB_RUNNING) {
        _setStatus("ERROR", true, "BUSY");
        return false;
    }

    _selected = index;
    if (!_loadSelectedScript()) {
        return false;
    }

    _resetExecution();
    _state = BADUSB_ARMED;
    _updateSelectedState();
    _setStatus("ARMED");

    STATE_WRITE_BEGIN();
    g_state.badUsbArmed = true;
    g_state.badUsbRunning = false;
    g_state.badUsbCountdownMs = 0;
    g_state.badUsbProgressLine = 0;
    g_state.dataRefresh = true;
    STATE_WRITE_END();

    DLOG_INFO("BADUSB", "Armed script idx=%d name=%s",
              _selected,
              _scripts[_selected].name);
    return true;
}

bool BadUsbManager::runArmed() {
    if (_state != BADUSB_ARMED || _armedScript.isEmpty()) {
        _setStatus("ERROR", true, "NOT ARMED");
        return false;
    }

    _resetExecution();
    _state = BADUSB_COUNTDOWN;
    _countdownStartMs = millis();
    _setStatus("COUNTDOWN");

    STATE_WRITE_BEGIN();
    g_state.badUsbArmed = true;
    g_state.badUsbRunning = true;
    g_state.badUsbCountdownMs = static_cast<uint16_t>(COUNTDOWN_MS);
    g_state.badUsbProgressLine = 0;
    g_state.dataRefresh = true;
    STATE_WRITE_END();

    return true;
}

void BadUsbManager::tick() {
    const uint32_t now = millis();

    switch (_state) {
        case BADUSB_COUNTDOWN:
            _tickCountdown(now);
            break;
        case BADUSB_RUNNING:
            _tickRunning(now);
            break;
        default:
            break;
    }
}

void BadUsbManager::cancel() {
    badUsbReleaseAll();
    _resetExecution();
    _armedScript = "";
    _state = BADUSB_IDLE;
    _setStatus("CANCEL");
    _updateSelectedState(_scriptCount <= 0);

    STATE_WRITE_BEGIN();
    g_state.badUsbArmed = false;
    g_state.badUsbRunning = false;
    g_state.badUsbCountdownMs = 0;
    g_state.badUsbProgressLine = 0;
    g_state.dataRefresh = true;
    STATE_WRITE_END();
}

bool BadUsbManager::_loadSelectedScript() {
    if (_selected < 0 || _selected >= _scriptCount) {
        _setStatus("ERROR", true, "NO SCRIPT");
        return false;
    }

    String body;
    if (!STORAGE.readBadUsbScript(_scripts[_selected].file, body)) {
        _setStatus("ERROR", true, "READ FAIL");
        return false;
    }

    _armedScript = body;
    return true;
}

void BadUsbManager::_tickCountdown(uint32_t now) {
    const uint32_t elapsed = now - _countdownStartMs;
    const uint16_t remain =
        (elapsed >= COUNTDOWN_MS) ? 0
                                  : static_cast<uint16_t>(COUNTDOWN_MS - elapsed);

    STATE_WRITE_BEGIN();
    g_state.badUsbCountdownMs = remain;
    g_state.dataRefresh = true;
    STATE_WRITE_END();

    if (elapsed < COUNTDOWN_MS) {
        return;
    }

    if (!badUsbHostReady()) {
        _failRun("USB NOT READY");
        return;
    }

    _state = BADUSB_RUNNING;
    _setStatus("RUNNING");
    _nextActionMs = now;
}

void BadUsbManager::_tickRunning(uint32_t now) {
    if (now < _nextActionMs) {
        return;
    }

    if (_releasePending) {
        badUsbReleaseAll();
        _releasePending = false;
        _nextActionMs = now + LINE_GAP_MS;
        return;
    }

    if (_pendingStringIndex < _pendingString.length()) {
        if (!badUsbHostReady()) {
            _failRun("USB LOST");
            return;
        }

        const uint8_t ch =
            static_cast<uint8_t>(_pendingString.charAt(_pendingStringIndex++));
        if (!badUsbWrite(ch)) {
            char err[48];
            snprintf(err, sizeof(err), "LINE %u BAD CHAR",
                     static_cast<unsigned>(_currentLine));
            _failRun(err);
            return;
        }

        if (_pendingStringIndex >= _pendingString.length()) {
            _pendingString = "";
            _pendingStringIndex = 0;
            _nextActionMs = now + LINE_GAP_MS;
        } else {
            _nextActionMs = now + STRING_CHAR_DELAY_MS;
        }
        return;
    }

    while (true) {
        String line;
        uint16_t lineNo = 0;
        if (!_readNextLine(line, lineNo)) {
            _finishRun();
            return;
        }

        const BadUsbStepResult step = _queueLine(line, lineNo, now);
        if (step == BADUSB_STEP_WAIT || step == BADUSB_STEP_ERROR) {
            return;
        }
    }
}

bool BadUsbManager::_readNextLine(String& outLine, uint16_t& outLineNo) {
    if (_scriptCursor >= _armedScript.length()) {
        return false;
    }

    int end = _armedScript.indexOf('\n', _scriptCursor);
    if (end < 0) {
        end = _armedScript.length();
    }

    outLine = _armedScript.substring(_scriptCursor, end);
    if (outLine.endsWith("\r")) {
        outLine.remove(outLine.length() - 1);
    }
    outLine.trim();

    _scriptCursor = end + 1;
    outLineNo = ++_currentLine;

    STATE_WRITE_BEGIN();
    g_state.badUsbProgressLine = outLineNo;
    g_state.dataRefresh = true;
    STATE_WRITE_END();

    return true;
}

BadUsbStepResult BadUsbManager::_queueLine(const String& line,
                                           uint16_t lineNo,
                                           uint32_t now) {
    if (line.isEmpty()) {
        return BADUSB_STEP_NOOP;
    }

    const String upper = uppercaseToken(line);
    if (upper == "REM" || upper.startsWith("REM ")) {
        return BADUSB_STEP_NOOP;
    }

    if (upper == "DELAY" || upper.startsWith("DELAY ")) {
        const String arg = (line.length() > 5) ? line.substring(5) : String();
        uint16_t delayMs = 0;
        if (!parseUint16Strict(arg, delayMs)) {
            char err[48];
            snprintf(err, sizeof(err), "LINE %u BAD DELAY",
                     static_cast<unsigned>(lineNo));
            _failRun(err);
            return BADUSB_STEP_ERROR;
        }

        if (delayMs == 0) {
            return BADUSB_STEP_NOOP;
        }

        _nextActionMs = now + delayMs;
        return BADUSB_STEP_WAIT;
    }

    if (upper == "STRING") {
        char err[48];
        snprintf(err, sizeof(err), "LINE %u NEEDS TEXT",
                 static_cast<unsigned>(lineNo));
        _failRun(err);
        return BADUSB_STEP_ERROR;
    }

    if (upper.startsWith("STRING ")) {
        _pendingString = line.substring(7);
        _pendingStringIndex = 0;

        if (_pendingString.isEmpty()) {
            return BADUSB_STEP_NOOP;
        }

        _nextActionMs = now;
        return BADUSB_STEP_WAIT;
    }

    if (!_queueCombo(line, lineNo, now)) {
        return BADUSB_STEP_ERROR;
    }

    return BADUSB_STEP_WAIT;
}

bool BadUsbManager::_queueCombo(const String& line,
                                uint16_t lineNo,
                                uint32_t now) {
    uint8_t keys[6] = {};
    int keyCount = 0;

    int start = 0;
    while (start < line.length()) {
        while (start < line.length() && line.charAt(start) == ' ') {
            ++start;
        }
        if (start >= line.length()) {
            break;
        }

        int end = line.indexOf(' ', start);
        if (end < 0) {
            end = line.length();
        }

        const String token = line.substring(start, end);
        uint8_t key = 0;
        if (!_tokenToKey(token, key)) {
            char err[48];
            snprintf(err, sizeof(err), "LINE %u UNKNOWN: %.20s",
                     static_cast<unsigned>(lineNo),
                     uppercaseToken(token).c_str());
            _failRun(err);
            return false;
        }

        if (keyCount >= 6) {
            char err[48];
            snprintf(err, sizeof(err), "LINE %u TOO MANY KEYS",
                     static_cast<unsigned>(lineNo));
            _failRun(err);
            return false;
        }

        keys[keyCount++] = key;
        start = end + 1;
    }

    if (keyCount <= 0) {
        return true;
    }

    if (!badUsbHostReady()) {
        _failRun("USB NOT READY");
        return false;
    }

    for (int i = 0; i < keyCount; ++i) {
        if (!badUsbPress(keys[i])) {
            badUsbReleaseAll();
            char err[48];
            snprintf(err, sizeof(err), "LINE %u KEY FAIL",
                     static_cast<unsigned>(lineNo));
            _failRun(err);
            return false;
        }
    }

    _releasePending = true;
    _nextActionMs = now + CHORD_HOLD_MS;
    return true;
}

bool BadUsbManager::_tokenToKey(const String& token, uint8_t& outKey) const {
    const String upper = uppercaseToken(token);
    if (upper.isEmpty()) {
        return false;
    }

    if (upper == "ENTER" || upper == "RETURN") {
        outKey = KEY_RETURN;
        return true;
    }
    if (upper == "TAB") {
        outKey = KEY_TAB;
        return true;
    }
    if (upper == "ESC" || upper == "ESCAPE") {
        outKey = KEY_ESC;
        return true;
    }
    if (upper == "UP" || upper == "UPARROW") {
        outKey = KEY_UP_ARROW;
        return true;
    }
    if (upper == "DOWN" || upper == "DOWNARROW") {
        outKey = KEY_DOWN_ARROW;
        return true;
    }
    if (upper == "LEFT" || upper == "LEFTARROW") {
        outKey = KEY_LEFT_ARROW;
        return true;
    }
    if (upper == "RIGHT" || upper == "RIGHTARROW") {
        outKey = KEY_RIGHT_ARROW;
        return true;
    }
    if (upper == "GUI" || upper == "WIN" || upper == "WINDOWS" ||
        upper == "CMD" || upper == "COMMAND") {
        outKey = KEY_LEFT_GUI;
        return true;
    }
    if (upper == "CTRL" || upper == "CONTROL") {
        outKey = KEY_LEFT_CTRL;
        return true;
    }
    if (upper == "ALT" || upper == "OPTION") {
        outKey = KEY_LEFT_ALT;
        return true;
    }
    if (upper == "SHIFT") {
        outKey = KEY_LEFT_SHIFT;
        return true;
    }
    if (upper == "SPACE" || upper == "SPACEBAR") {
        outKey = KEY_SPACE;
        return true;
    }
    if (upper == "BACKSPACE" || upper == "BKSP") {
        outKey = KEY_BACKSPACE;
        return true;
    }
    if (upper == "DELETE" || upper == "DEL") {
        outKey = KEY_DELETE;
        return true;
    }
    if (upper == "HOME") {
        outKey = KEY_HOME;
        return true;
    }
    if (upper == "END") {
        outKey = KEY_END;
        return true;
    }
    if (upper == "PAGEUP") {
        outKey = KEY_PAGE_UP;
        return true;
    }
    if (upper == "PAGEDOWN") {
        outKey = KEY_PAGE_DOWN;
        return true;
    }
    if (upper == "INSERT") {
        outKey = KEY_INSERT;
        return true;
    }

    if (upper.length() == 1) {
        char ch = token.charAt(0);
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
        outKey = static_cast<uint8_t>(ch);
        return true;
    }

    if (upper.charAt(0) == 'F') {
        const int fn = upper.substring(1).toInt();
        switch (fn) {
            case 1:  outKey = KEY_F1;  return true;
            case 2:  outKey = KEY_F2;  return true;
            case 3:  outKey = KEY_F3;  return true;
            case 4:  outKey = KEY_F4;  return true;
            case 5:  outKey = KEY_F5;  return true;
            case 6:  outKey = KEY_F6;  return true;
            case 7:  outKey = KEY_F7;  return true;
            case 8:  outKey = KEY_F8;  return true;
            case 9:  outKey = KEY_F9;  return true;
            case 10: outKey = KEY_F10; return true;
            case 11: outKey = KEY_F11; return true;
            case 12: outKey = KEY_F12; return true;
            default: break;
        }
    }

    return false;
}

bool BadUsbManager::_updateSelectedState(bool placeholder) {
    STATE_WRITE_BEGIN();
    g_state.badUsbScriptCount = _scriptCount;
    g_state.badUsbReady = _ready && (_scriptCount > 0);

    if (placeholder || _selected < 0 || _selected >= _scriptCount) {
        strlcpy(g_state.badUsbScriptName, "NO SCRIPTS", sizeof(g_state.badUsbScriptName));
        strlcpy(g_state.badUsbScriptDesc, "LOAD VIA VAULT", sizeof(g_state.badUsbScriptDesc));
        g_state.badUsbListSelected = 0;
        g_state.badUsbListScroll = 0;
    } else {
        strlcpy(g_state.badUsbScriptName, _scripts[_selected].name,
                sizeof(g_state.badUsbScriptName));
        strlcpy(g_state.badUsbScriptDesc, _scripts[_selected].desc,
                sizeof(g_state.badUsbScriptDesc));
        g_state.badUsbListSelected = _selected;
        if (g_state.badUsbListScroll > _selected) {
            g_state.badUsbListScroll = _selected;
        }
    }

    g_state.dataRefresh = true;
    STATE_WRITE_END();
    return !placeholder;
}

void BadUsbManager::_resetExecution() {
    _countdownStartMs = 0;
    _nextActionMs = 0;
    _scriptCursor = 0;
    _currentLine = 0;
    _pendingString = "";
    _pendingStringIndex = 0;
    _releasePending = false;
    badUsbReleaseAll();
}

void BadUsbManager::_finishRun() {
    _resetExecution();
    _armedScript = "";
    _state = BADUSB_COMPLETE;
    _setStatus("DONE");

    STATE_WRITE_BEGIN();
    g_state.badUsbArmed = false;
    g_state.badUsbRunning = false;
    g_state.badUsbCountdownMs = 0;
    g_state.dataRefresh = true;
    STATE_WRITE_END();
}

void BadUsbManager::_failRun(const char* detail) {
    _resetExecution();
    _armedScript = "";
    _state = BADUSB_ERROR;
    _setStatus("ERROR", true, detail);

    STATE_WRITE_BEGIN();
    g_state.badUsbArmed = false;
    g_state.badUsbRunning = false;
    g_state.badUsbCountdownMs = 0;
    g_state.dataRefresh = true;
    STATE_WRITE_END();
}

void BadUsbManager::_setStatus(const char* status, bool error, const char* detail) {
    STATE_WRITE_BEGIN();
    strlcpy(g_state.badUsbStatus, status ? status : "?",
            sizeof(g_state.badUsbStatus));
    g_state.badUsbHasError = error;
    if (detail) {
        strlcpy(g_state.badUsbError, detail, sizeof(g_state.badUsbError));
    } else {
        g_state.badUsbError[0] = '\0';
    }
    g_state.dataRefresh = true;
    STATE_WRITE_END();

    if (error) {
        DLOG_WARN("BADUSB", "%s", detail ? detail : "error");
    } else {
        DLOG_INFO("BADUSB", "%s", status ? status : "?");
    }
}




