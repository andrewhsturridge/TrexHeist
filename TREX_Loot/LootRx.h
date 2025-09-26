#pragma once
#include <stdint.h>

// RX entry point used by Transport::init(cfg, onRx)
void onRx(const uint8_t* data, uint16_t len);
