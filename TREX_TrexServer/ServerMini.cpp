#include "ServerMini.h"
#include "GameModel.h"   // now include the full Game definition in the .cpp
#include "Net.h"         // bcastMgStart/bcastMgStop, bcastScore
#include <Arduino.h>
#include <esp_random.h>

// Award hook – swap to your real scorer if needed
static void awardBonusPoint(Game& g, const TrexUid& uid) {
  (void)uid;
  g.teamScore += 1;
  bcastScore(g);
}

void MG_Init(Game& g) {
  g.mgActive           = false;
  g.mgStartedAt        = 0;
  g.mgDeadline         = 0;
  g.mgAllTriedAt       = 0;
  g.mgTriedMask        = 0;
  g.mgSuccessMask      = 0;
  g.mgExpectedStations = 5;  // adjust to your Loot count
  g.mgCfg = Game::MgConfig{};
}

void MG_Start(Game& g, const MgConfig& cfg, uint32_t nowMs) {
  if (g.mgActive) return;

  // Copy config into Game::mgCfg with safe defaults
  g.mgCfg.seed       = cfg.seed       ? cfg.seed       : esp_random();
  g.mgCfg.timerMs    = cfg.timerMs    ? cfg.timerMs    : 60000;
  g.mgCfg.speedMinMs = cfg.speedMinMs ? cfg.speedMinMs : 20;
  g.mgCfg.speedMaxMs = cfg.speedMaxMs ? cfg.speedMaxMs : 80;
  g.mgCfg.segMin     = cfg.segMin     ? cfg.segMin     : 6;
  g.mgCfg.segMax     = cfg.segMax     ? cfg.segMax     : 16;

  g.mgActive     = true;
  g.mgStartedAt  = nowMs;
  g.mgDeadline   = nowMs + g.mgCfg.timerMs;
  g.mgAllTriedAt = 0;
  g.mgTriedMask  = 0;
  g.mgSuccessMask= 0;

  // Broadcast MG_START using your Net helper
  bcastMgStart(g, g.mgCfg);

  Serial.printf("[MG] START seed=%lu timer=%u speed=%u..%u seg=%u..%u\n",
    (unsigned long)g.mgCfg.seed, g.mgCfg.timerMs,
    g.mgCfg.speedMinMs, g.mgCfg.speedMaxMs, g.mgCfg.segMin, g.mgCfg.segMax);
}

bool MG_Tick(Game& g, uint32_t nowMs) {
  if (!g.mgActive) return false;

  if ((int32_t)(nowMs - g.mgDeadline) >= 0) { MG_Stop(g, nowMs); return false; }
  if (g.mgAllTriedAt && (int32_t)(nowMs - (g.mgAllTriedAt + 3000)) >= 0) {
    MG_Stop(g, nowMs); return false;
  }
  return true; // still running
}

void MG_OnResult(Game& g, const MgResultPayload& r, uint32_t nowMs) {
  if (!g.mgActive) return;
  if (r.stationId == 0 || r.stationId >= 32) return;

  const uint32_t bit = (1u << r.stationId);
  if (g.mgTriedMask & bit) {
    Serial.printf("[MG] duplicate result sid=%u ignored\n", r.stationId);
    return;
  }

  g.mgTriedMask |= bit;
  if (r.success) {
    g.mgSuccessMask |= bit;
    awardBonusPoint(g, r.uid);
  }

  // Count bits set
  uint32_t m = g.mgTriedMask; uint8_t cnt = 0;
  while (m) { cnt += (m & 1u); m >>= 1; }
  if (cnt >= g.mgExpectedStations && g.mgAllTriedAt == 0) g.mgAllTriedAt = nowMs;

  Serial.printf("[MG] result sid=%u success=%u triedMask=%08lx\n",
    r.stationId, r.success ? 1 : 0, (unsigned long)g.mgTriedMask);
}

void MG_Stop(Game& g, uint32_t /*nowMs*/) {
  if (!g.mgActive) return;
  g.mgActive = false;
  bcastMgStop(g);
  Serial.println("[MG] STOP");
}
