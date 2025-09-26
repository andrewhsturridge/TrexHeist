#pragma once
#include <stdint.h>
#include <TrexProtocol.h>  // for MgResultPayload

// Forward declare to avoid include cycles.
struct Game;

// Lightweight config for starting MG from server code
struct MgConfig {
  uint32_t seed;
  uint16_t timerMs;
  uint8_t  speedMinMs, speedMaxMs;
  uint8_t  segMin, segMax;
};

void MG_Init(Game& g);
void MG_Start(Game& g, const MgConfig& cfg, uint32_t nowMs);
bool MG_Tick(Game& g, uint32_t nowMs);
void MG_OnResult(Game& g, const MgResultPayload& r, uint32_t nowMs);
void MG_Stop(Game& g, uint32_t nowMs);
