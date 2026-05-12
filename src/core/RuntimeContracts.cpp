


#include "RuntimeContracts.h"

#include <cstdarg>
#include <cstdio>

#include "DebugLog.h"

bool RuntimeContracts::_reported[CONTRACT_COUNT] = {};
portMUX_TYPE RuntimeContracts::_mux = portMUX_INITIALIZER_UNLOCKED;

void RuntimeContracts::noteViolation(RuntimeContractId id,
                                     const char* area,
                                     const char* fmt, ...) {
    if (id >= CONTRACT_COUNT) {
        return;
    }

    bool shouldLog = false;
    portENTER_CRITICAL(&_mux);
    if (!_reported[id]) {
        _reported[id] = true;
        shouldLog = true;
    }
    portEXIT_CRITICAL(&_mux);

    if (!shouldLog) {
        return;
    }

    char detail[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);

    DLOG_ERROR(area ? area : "CONTRACT", "Contract violation: %s", detail);
}

void RuntimeContracts::clear(RuntimeContractId id) {
    if (id >= CONTRACT_COUNT) {
        return;
    }

    portENTER_CRITICAL(&_mux);
    _reported[id] = false;
    portEXIT_CRITICAL(&_mux);
}




