

#pragma once

#include <Arduino.h>

class AntennaManager {
public:
    void begin(bool externalDefault);
    bool setExternal(bool external);
    bool toggle();

    bool isAvailable() const { return _available; }
    bool isExternal() const  { return _external; }
    const char* label() const;

private:
    bool _apply(bool external);

    bool _available = false;
    bool _external  = true;
};



