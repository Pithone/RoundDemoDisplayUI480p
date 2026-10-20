#include "board_io.h"

#include <Arduino.h>
#include <Wire.h>

#include "board_config.h"

namespace {

uint8_t outputState = 0;

bool writeRegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(board::kExpanderAddress);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

}  // namespace

bool expanderInit() {
    constexpr uint8_t outputMask =
        (1U << board::kLcdResetBit) |
        (1U << board::kTouchResetBit) |
        (1U << board::kLcdChipSelectBit) |
        (1U << board::kSdD3EnableBit) |
        (1U << board::kBuzzerBit);

    outputState = (1U << board::kLcdResetBit) |
                  (1U << board::kTouchResetBit) |
                  (1U << board::kLcdChipSelectBit) |
                  (1U << board::kSdD3EnableBit);

    if (!writeRegister(board::kExpanderOutputRegister, outputState)) {
        return false;
    }
    if (!writeRegister(board::kExpanderConfigRegister,
                       static_cast<uint8_t>(~outputMask))) {
        return false;
    }
    return true;
}

bool expanderWriteBit(uint8_t bit, bool high) {
    if (bit > 7) {
        return false;
    }
    const uint8_t mask = static_cast<uint8_t>(1U << bit);
    const uint8_t nextState = high
        ? static_cast<uint8_t>(outputState | mask)
        : static_cast<uint8_t>(outputState & ~mask);

    if (!writeRegister(board::kExpanderOutputRegister, nextState)) {
        return false;
    }
    outputState = nextState;
    return true;
}

void setBuzzer(bool on) {
    if (!expanderWriteBit(board::kBuzzerBit, on)) {
        Serial.println("[ERROR] TCA9554 buzzer write failed");
    }
}
