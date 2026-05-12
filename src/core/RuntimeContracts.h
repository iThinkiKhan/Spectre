


#pragma once

#include <Arduino.h>

enum RuntimeContractId : uint8_t {
    CONTRACT_MODE_OWNER_SYNC = 0,
    CONTRACT_PWNY_OWNER_SYNC,
    CONTRACT_UPLOAD_OWNER_SYNC,
    CONTRACT_UPLOAD_BATCH_OWNER_SYNC,
    CONTRACT_STORAGE_BATCH_NESTING,
    CONTRACT_STORAGE_MARK_OUTSIDE_BATCH,
    CONTRACT_COUNT
};

class RuntimeContracts {
public:
    static void noteViolation(RuntimeContractId id,
                              const char* area,
                              const char* fmt, ...);
    static void clear(RuntimeContractId id);

private:
    static bool _reported[CONTRACT_COUNT];
    static portMUX_TYPE _mux;
};

#define CONTRACT_WARN_ONCE(id, area, condition, fmt, ...)           \
    do {                                                            \
        if (condition) {                                            \
            RuntimeContracts::clear(id);                            \
        } else {                                                    \
            RuntimeContracts::noteViolation(id, area, fmt,          \
                                            ##__VA_ARGS__);         \
        }                                                           \
    } while (0)




