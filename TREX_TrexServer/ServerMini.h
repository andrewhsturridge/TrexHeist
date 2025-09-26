#pragma once
#include <stdint.h>
#include <TrexProtocol.h>

struct Game;  // forward-declare your Game state

// Configuration broadcast to clients
struct MgConfig {
  uint32_t seed;
  uint16_t timerMs;       // e.g., 60000
  uint8_t  speedMinMs;    // e.g., 20
  uint8_t  speedMaxMs;    // e.g., 80
  uint8_t  segMin;        // e.g., 6
  uint8_t  segMax;        // e.g., 16
};

// Server-held MG state
struct MgState {
  bool     active = false;
  uint32_t startedAt = 0;        // ms
  uint32_t deadline  = 0;        // ms (start + timer)
  uint32_t allUsedAt = 0;        // ms (first time everyone has tried)

  // bitmasks indexed by stationId (1..31) -> bit (1 << stationId)
  uint32_t triedMask   = 0;      // stations that have already had their one try
  uint32_t successMask = 0;      // stations that succeeded
  uint32_t resultMask  = 0;      // stations that replied (success|fail)

  // We can bound the expected participants (Loot stations only)
  uint8_t  expectedStations = 5; // adjust to your deployment (1..31)

  // Copy of the broadcast config
  MgConfig cfg{};
};

// Initialize/clear the MG subsystem (call once at server boot)
void MG_Init(Game& g);

// Start the MG round (server decides when—e.g., after Round 4)
void MG_Start(Game& g, const MgConfig& cfg, uint32_t nowMs);

// Tick; returns true if MG consumed the state progression (i.e., still running)
bool MG_Tick(Game& g, uint32_t nowMs);

// Handle client result (call from Net RX on MsgType::MG_RESULT)
void MG_OnResult(Game& g, const MgResultPayload& r, uint32_t nowMs);

// Force stop + broadcast MG_STOP (used on timeout or after all tried +3s)
void MG_Stop(Game& g, uint32_t nowMs);
