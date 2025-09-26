#pragma once
#include <stdint.h>
#include <TrexProtocol.h>   // TrexUid

// Public flags owned by the .ino (so everyone can gate on it)
extern volatile bool mgActive;   // minigame owns the gauge when true

// Start/stop from RX
struct MgParams {
  uint32_t seed;
  uint16_t timerMs;
  uint8_t  speedMinMs, speedMaxMs;
  uint8_t  segMin, segMax;
};

void mgStart(const MgParams& p); // called by RX on MG_START
void mgStop();                   // called by RX on MG_STOP (or GAME_OVER/START)

// Loop to run while mgActive; safe to call every loop() tick
void mgLoop();

// (Used by RX upon a station-wide reset conditions)
void mgCancel(); // same as mgStop() but without redraws
