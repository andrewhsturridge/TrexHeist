#pragma once
#include <Arduino.h>

// Track IDs (your SD filenames 00001..00006)
constexpr uint16_t TRK_PLAYERS_STAY_STILL = 1;  // 00001
constexpr uint16_t TRK_TICKS              = 2;  // 00002 (unused)
constexpr uint16_t TRK_TICKS_LOOP         = 3;  // 00003
constexpr uint16_t TRK_TREX_LOSE          = 4;  // 00004
constexpr uint16_t TRK_TREX_WIN           = 5;  // 00005
constexpr uint16_t TRK_GAME_MUSIC         = 6;  // 00006 (unused)

// Init + controls
void gameAudioInit(uint8_t rxPin = 9, uint8_t txPin = 8, uint32_t baud = 9600, uint8_t volume = 25);
void gameAudioPlayOnce(uint16_t track);
void gameAudioStop();
uint16_t gameAudioCurrentTrack();
