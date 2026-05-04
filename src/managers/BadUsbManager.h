

#pragma once

#include <Arduino.h>
#include "StorageManager.h"

enum BadUsbExecState : uint8_t {
    BADUSB_IDLE = 0,
    BADUSB_ARMED,
    BADUSB_COUNTDOWN,
    BADUSB_RUNNING,
    BADUSB_COMPLETE,
    BADUSB_ERROR
};

enum BadUsbStepResult : uint8_t {
    BADUSB_STEP_NOOP = 0,
    BADUSB_STEP_WAIT,
    BADUSB_STEP_ERROR
};

class BadUsbManager {
public:
    static BadUsbManager& getInstance() {
        static BadUsbManager instance;
        return instance;
    }

    bool begin();
    void tick();

    int  refreshScripts();
    int  scriptCount() const { return _scriptCount; }
    const BadUsbScriptInfo* scripts() const { return _scripts; }

    bool selectScript(int index);
    bool armSelected(int index);
    bool runArmed();
    void cancel();

    bool isReady() const { return _ready; }
    bool isRunning() const { return _state == BADUSB_RUNNING || _state == BADUSB_COUNTDOWN; }
    BadUsbExecState state() const { return _state; }

private:
    BadUsbManager() = default;

    static constexpr int MAX_SCRIPTS = 16;
    static constexpr uint32_t COUNTDOWN_MS = 3000UL;
    static constexpr uint32_t CHORD_HOLD_MS = 32UL;
    static constexpr uint32_t STRING_CHAR_DELAY_MS = 12UL;
    static constexpr uint32_t LINE_GAP_MS = 18UL;

    BadUsbScriptInfo _scripts[MAX_SCRIPTS] = {};
    int _scriptCount = 0;
    int _selected = -1;
    bool _ready = false;
    BadUsbExecState _state = BADUSB_IDLE;
    uint32_t _countdownStartMs = 0;
    uint32_t _nextActionMs = 0;
    int _scriptCursor = 0;
    uint16_t _currentLine = 0;
    String _armedScript;
    String _pendingString;
    size_t _pendingStringIndex = 0;
    bool _releasePending = false;

    bool _loadSelectedScript();
    void _tickCountdown(uint32_t now);
    void _tickRunning(uint32_t now);
    bool _readNextLine(String& outLine, uint16_t& outLineNo);
    BadUsbStepResult _queueLine(const String& line, uint16_t lineNo, uint32_t now);
    bool _queueCombo(const String& line, uint16_t lineNo, uint32_t now);
    bool _tokenToKey(const String& token, uint8_t& outKey) const;
    bool _updateSelectedState(bool placeholder = false);
    void _resetExecution();
    void _finishRun();
    void _failRun(const char* detail);
    void _setStatus(const char* status, bool error = false, const char* detail = nullptr);
};

#define BADUSB_MGR BadUsbManager::getInstance()



