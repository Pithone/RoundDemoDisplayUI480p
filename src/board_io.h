#pragma once

#include <stdint.h>

bool expanderInit();
bool expanderWriteBit(uint8_t bit, bool high);
void setBuzzer(bool on);
