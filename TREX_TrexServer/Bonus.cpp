#include "Bonus.h"
#include "Net.h"
#include <Arduino.h>

static inline uint32_t jittered(uint32_t mean, uint32_t jitter) {
  if (!jitter) return mean;
  int32_t off = (int32_t)random(-(int32_t)jitter, (int32_t)jitter + 1);
  int32_t v = (int32_t)mean + off;
  return (v < 500) ? 500u : (uint32_t)v;
}

struct BonusParams {
  bool     kAll;
  uint8_t  maxConcurrent;
  uint8_t  maxSpawnsPerRound;
  uint32_t durationMs;
  uint32_t intervalMeanMs;
  uint32_t intervalJitterMs;
};

static inline BonusParams paramsForRound(uint8_t r) {
  BonusParams p{};
  if (r == 3) {
    p.kAll = true;  p.maxConcurrent = 5; p.maxSpawnsPerRound = 3;
    p.durationMs = 12000; p.intervalMeanMs = 45000; p.intervalJitterMs = 10000;
  } else { // r == 4 (only rounds 3/4 spawn)
    p.kAll = false; p.maxConcurrent = 1; p.maxSpawnsPerRound = 3;
    p.durationMs = 10000; p.intervalMeanMs = 35000; p.intervalJitterMs =  8000;
  }
  return p;
}

void bonusResetForRound(Game& g, uint32_t now) {
  g.bonusActiveMask = 0;
  for (int i=1;i<=MAX_STATIONS;++i) g.bonusEndsAt[i] = 0;
  g.bonusSpawnsThisRound = 0;
  if (g.roundIndex == 3 || g.roundIndex == 4) {
    auto p = paramsForRound(g.roundIndex);
    g.bonusNextSpawnAt = now + jittered(p.intervalMeanMs, p.intervalJitterMs);
  } else {
    g.bonusNextSpawnAt = 0;
  }
  bcastBonusUpdate(g); // clear any stale UI
}

static void spawnNow(Game& g, uint32_t now, const BonusParams& p, bool obeyCap=true) {
  // Respect concurrency cap
  if (obeyCap) {
    uint32_t m = g.bonusActiveMask; uint8_t active=0; while (m){ m&=(m-1); ++active; }
    if (active >= p.maxConcurrent) return;
  }

  // Eligible set (skip empty + already bonus)
  uint8_t elig[MAX_STATIONS]; uint8_t eCount=0;
  for (uint8_t sid=1; sid<=MAX_STATIONS; ++sid) {
    if ((g.bonusActiveMask & (1u<<sid)) == 0 && g.stationInventory[sid] > 0) {
      elig[eCount++] = sid;
    }
  }
  if (eCount == 0) return;

  if (p.kAll) {
    for (uint8_t i=0;i<eCount;++i) {
      const uint8_t sid = elig[i];
      g.bonusActiveMask |= (1u<<sid);
      g.bonusEndsAt[sid] = now + p.durationMs;
    }
  } else {
    const uint8_t sid = elig[(uint8_t)random(0, eCount)];
    g.bonusActiveMask |= (1u<<sid);
    g.bonusEndsAt[sid] = now + p.durationMs;
  }

  ++g.bonusSpawnsThisRound;
  bcastBonusUpdate(g);
}

void tickBonusDirector(Game& g, uint32_t now) {
  // Expire finished or empty immediately
  bool dirty=false;
  for (uint8_t sid=1; sid<=MAX_STATIONS; ++sid) {
    if (g.bonusActiveMask & (1u<<sid)) {
      const bool ttlOver = (g.bonusEndsAt[sid] > 0) && (now >= g.bonusEndsAt[sid]);
      const bool empty   = (g.stationInventory[sid] == 0);
      if (ttlOver || empty) {
        g.bonusActiveMask &= ~(1u<<sid);
        g.bonusEndsAt[sid] = 0;
        dirty = true;
      }
    }
  }
  if (dirty) bcastBonusUpdate(g);

  if (g.phase != Phase::PLAYING) return;
  if (!(g.roundIndex == 3 || g.roundIndex == 4)) return;

  BonusParams p = paramsForRound(g.roundIndex);
  if (g.bonusSpawnsThisRound >= p.maxSpawnsPerRound) return;
  if (g.bonusNextSpawnAt == 0 || now < g.bonusNextSpawnAt) return;

  spawnNow(g, now, p, /*obeyCap=*/true);
  g.bonusNextSpawnAt = now + jittered(p.intervalMeanMs, p.intervalJitterMs);
}

void bonusForceSpawn(Game& g, uint32_t now) {
  if (!(g.roundIndex == 3 || g.roundIndex == 4)) { Serial.println("[BONUS] force ignored (not R3/R4)"); return; }
  Serial.println("[BONUS] Starting");
  BonusParams p = paramsForRound(g.roundIndex);
  spawnNow(g, now, p, /*obeyCap=*/true);
  // do not advance counters/timers here; manual trigger is ad-hoc
}

void bonusClearAll(Game& g) {
  g.bonusActiveMask = 0;
  for (int i=1;i<=MAX_STATIONS;++i) g.bonusEndsAt[i] = 0;
  bcastBonusUpdate(g);
}

// Effect: instant-drain on hold start (no overflow discard)
bool applyBonusOnHoldStart(Game& g, uint8_t playerIdx, uint8_t stationId, uint32_t holdId) {
  if ((g.bonusActiveMask & (1u<<stationId)) == 0) return false;
  uint32_t inv = g.stationInventory[stationId];
  if (inv == 0) return false;

  auto &pl = g.players[playerIdx];
  if (pl.carried >= g.maxCarry) { sendHoldEnd(g, holdId, /*FULL*/0); return true; }

  uint32_t space = g.maxCarry - pl.carried;
  uint32_t take  = (inv > space) ? space : inv;

  pl.carried += take;
  g.stationInventory[stationId] = inv - take;

  sendLootTick(g, holdId, pl.carried, g.stationInventory[stationId]);
  bcastStation(g, stationId);

  if (g.stationInventory[stationId] == 0) {
    g.bonusActiveMask &= ~(1u<<stationId);
    g.bonusEndsAt[stationId] = 0;
    bcastBonusUpdate(g);
  }

  if (pl.carried >= g.maxCarry) sendHoldEnd(g, holdId, /*FULL*/0);
  else                          sendHoldEnd(g, holdId, /*EMPTY*/1);
  return true;
}
