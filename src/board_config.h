#pragma once

#include <driver/gpio.h>
#include <driver/spi_master.h>

namespace board {

constexpr int kWidth = 480;
constexpr int kHeight = 480;

constexpr gpio_num_t kI2cSda = GPIO_NUM_15;
constexpr gpio_num_t kI2cScl = GPIO_NUM_7;
constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_16;
constexpr uint8_t kTouchAddress = 0x5D;
constexpr uint16_t kTouchStatusRegister = 0x814E;
constexpr uint16_t kTouchPointRegister = 0x8150;

constexpr uint8_t kExpanderAddress = 0x20;
constexpr uint8_t kExpanderInputRegister = 0x00;
constexpr uint8_t kExpanderOutputRegister = 0x01;
constexpr uint8_t kExpanderConfigRegister = 0x03;

constexpr uint8_t kLcdResetBit = 0;
constexpr uint8_t kTouchResetBit = 1;
constexpr uint8_t kLcdChipSelectBit = 2;
constexpr uint8_t kSdD3EnableBit = 3;  // TCA9554 EXIO pin 4 enables the SD_MMC D3 line.
constexpr uint8_t kBuzzerBit = 7;        // TCA9554 P7 / EXIO8, active high.

constexpr gpio_num_t kLcdBacklight = GPIO_NUM_6;
constexpr gpio_num_t kLcdCommandMosi = GPIO_NUM_1;
constexpr gpio_num_t kLcdCommandClock = GPIO_NUM_2;
constexpr spi_host_device_t kLcdCommandHost = SPI2_HOST;

constexpr gpio_num_t kSdCommand = GPIO_NUM_1;
constexpr gpio_num_t kSdData0 = GPIO_NUM_42;
constexpr gpio_num_t kSdClock = GPIO_NUM_2;

constexpr gpio_num_t kRgbData[16] = {
    GPIO_NUM_5,  GPIO_NUM_45, GPIO_NUM_48, GPIO_NUM_47,
    GPIO_NUM_21, GPIO_NUM_14, GPIO_NUM_13, GPIO_NUM_12,
    GPIO_NUM_11, GPIO_NUM_10, GPIO_NUM_9,  GPIO_NUM_46,
    GPIO_NUM_3,  GPIO_NUM_8,  GPIO_NUM_18, GPIO_NUM_17,
};
constexpr gpio_num_t kRgbHsync = GPIO_NUM_38;
constexpr gpio_num_t kRgbVsync = GPIO_NUM_39;
constexpr gpio_num_t kRgbDe = GPIO_NUM_40;
constexpr gpio_num_t kRgbPclk = GPIO_NUM_41;

constexpr uint32_t kPixelClockHz = 14'000'000;
constexpr uint32_t kRgbBounceBufferLines = 10;
constexpr uint8_t kHsyncPulse = 8;
constexpr uint8_t kHsyncBackPorch = 10;
constexpr uint8_t kHsyncFrontPorch = 50;
constexpr uint8_t kVsyncPulse = 2;
constexpr uint8_t kVsyncBackPorch = 18;
constexpr uint8_t kVsyncFrontPorch = 8;

constexpr uint32_t kBacklightPwmHz = 5000;
constexpr uint8_t kBacklightPwmBits = 10;
constexpr uint8_t kDefaultBrightness = 230;

}  // namespace board
