#pragma once

#include <Arduino.h>
#include <Servo.h>

#include "config.h"

// SG90 barrier arm. Positions are servo pulse widths in microseconds, calibrated per arm
// (see config.h). Closes automatically BARRIER_OPEN_MS after opening.
struct Barrier {
    const char* name;
    uint8_t servoPin;
    uint16_t openUs;
    uint16_t closedUs;
    Servo servo;
    bool open = false;
    uint32_t openedMs = 0;

    void begin() {
        servo.attach(servoPin, 400, 2600);
        close();
    }
    void openNow() {
        open = true;
        openedMs = millis();
        servo.writeMicroseconds(openUs);
        Serial.printf("%s offen\n", name);
    }
    void close() {
        open = false;
        servo.writeMicroseconds(closedUs);
    }
    void update() {
        if (open && millis() - openedMs >= BARRIER_OPEN_MS) {
            close();
            Serial.printf("%s geschlossen\n", name);
        }
    }
};
