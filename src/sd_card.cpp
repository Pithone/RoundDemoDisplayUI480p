#include "sd_card.h"

#include <Arduino.h>
#include <SD_MMC.h>

#include "board_config.h"
#include "board_io.h"

namespace {

constexpr char kMountPoint[] = "/sdcard";
bool fileSystemMounted = false;

}  // namespace

namespace sdcard {

bool mount(std::string& error) {
    if (fileSystemMounted) {
        return true;
    }
    error.clear();

    if (!expanderWriteBit(board::kSdD3EnableBit, true)) {
        error = "TCA9554 could not enable the SD card D3 line";
        Serial.printf("[SD] %s\n", error.c_str());
        return false;
    }

    if (!SD_MMC.setPins(board::kSdClock, board::kSdCommand, board::kSdData0)) {
        error = "Could not configure the SD_MMC pins";
        Serial.printf("[SD] %s\n", error.c_str());
        return false;
    }

    if (!SD_MMC.begin(kMountPoint, true, false)) {
        error = "SD_MMC initialization or FAT mount failed; check the card and FAT format";
        Serial.printf("[SD] %s\n", error.c_str());
        return false;
    }

    if (SD_MMC.cardType() == CARD_NONE) {
        SD_MMC.end();
        error = "No SD card detected";
        Serial.printf("[SD] %s\n", error.c_str());
        return false;
    }

    fileSystemMounted = true;
    Serial.printf("[SD] SD_MMC filesystem mounted at %s (%llu bytes)\n",
                  kMountPoint,
                  static_cast<unsigned long long>(SD_MMC.totalBytes()));
    return true;
}

bool isMounted() {
    return fileSystemMounted;
}

}  // namespace sdcard
