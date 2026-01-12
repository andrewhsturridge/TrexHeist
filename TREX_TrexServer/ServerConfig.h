#pragma once
#include <stdint.h>

// Radio / identity
constexpr uint8_t  DEFAULT_WIFI_CHANNEL = 6;
constexpr uint8_t  STATION_ID   = 0;   // server is always 0

// Board blue user LED on UM FeatherS3 (active-HIGH)
constexpr int BOARD_BLUE_LED = 13;

// PIR pins (active-LOW). Set to -1 to disable.
static int8_t PIN_PIR[4] = { 5, -1, -1, -1 };
constexpr uint32_t PIR_DEBOUNCE_MS = 60;

// Maintenance Wi-Fi
#define WIFI_SSID   "AndrewiPhone"
#define WIFI_PASS   "12345678"
#define HOSTNAME    "Trex-Server"

// Sprite serial pins / config
constexpr uint32_t SPRITE_BAUD = 9600;
constexpr int SPRITE_RX = 44;  // optional
constexpr int SPRITE_TX = 43;  // to Sprite RX
