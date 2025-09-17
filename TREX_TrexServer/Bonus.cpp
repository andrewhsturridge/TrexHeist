#include "Bonus.h"
#include "Net.h"          // bcastBonusUpdate, bcastStation, sendLootTick, sendHoldEnd
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
  } else {
    p.kAll = false; p.maxConcurrent = 1; p.maxSpawnsPerRound = 3;
    p.durationMs = 10000; p.intervalMeanMs = 35000; p.intervalJitterMs =  8000;
  }
  return p;
}

void bonusResetForRound(Game& g, uint32_t now) {
  g.bonusActiveMask = 0;
  for (int i=0; i<MAX_STATIONS; ++i) g.bonusEndsAt[i] = 0;
  g.bonusSpawnsThisRound = 0;
  if (g.roundIndex == 3 || g.roundIndex == 4) {
    auto p = paramsForRound(g.roundIndex);
    g.bonusNextSpawnAt = now + jittered(p.intervalMeanMs, p.intervalJitterMs);
  } else {
    g.bonusNextSpawnAt = 0;
  }
  bcastBonusUpdate(g); // clear UI state on clients
}

void tickBonusDirector(Game& g, uint32_t now) {
  // 1) Expire ended or empty stations immediately
  bool dirty = false;
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

  const BonusParams p = paramsForRound(g.roundIndex);

  // 2) Respect concurrency / per-round caps
  uint8_t active=0; { uint32_t m=g.bonusActiveMask; while (m) { m&=(m-1); ++active; } }
  if (active >= p.maxConcurrent) return;
  if (g.bonusSpawnsThisRound >= p.maxSpawnsPerRound) return;

  if (g.bonusNextSpawnAt == 0 || now < g.bonusNextSpawnAt) return;

  // 3) Build eligible set (skip empty and already-bonus)
  uint8_t elig[MAX_STATIONS]; uint8_t eCount=0;
  for (uint8_t sid=1; sid<=MAX_STATIONS; ++sid) {
    if ((g.bonusActiveMask & (1u<<sid)) == 0 && g.stationInventory[sid] > 0) {
      elig[eCount++] = sid;
    }
  }
  if (eCount == 0) { g.bonusNextSpawnAt = now + 3000; return; }

  // 4) Select stations (R3=all eligible, R4=1 random)
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
  g.bonusNextSpawnAt = now + jittered(p.intervalMeanMs, p.intervalJitterMs);
  bcastBonusUpdate(g);
}

// === “Instant drain” at hold start (no overflow discard) ===================
bool applyBonusOnHoldStart(Game& g, uint8_t playerIdx, uint8_t stationId, uint32_t holdId) {
  if ((g.bonusActiveMask & (1u<<stationId)) == 0) return false;     // not bonus
  uint32_t inv = g.stationInventory[stationId];
  if (inv == 0) return false;                                       // nothing to take

  auto &pl = g.players[playerIdx];
  if (pl.carried >= g.maxCarry) { sendHoldEnd(g, holdId, /*FULL*/0); return true; }

  uint32_t space = g.maxCarry - pl.carried;
  uint32_t take  = (inv > space) ? space : inv;

  pl.carried += take;
  g.stationInventory[stationId] = inv - take;   // remainder stays in station

  // Notify player + everyone else
  sendLootTick(g, holdId, pl.carried, g.stationInventory[stationId]);
  bcastStation(g, stationId);

  // If station emptied by this drain, end its bonus immediately
  if (g.stationInventory[stationId] == 0) {
    g.bonusActiveMask &= ~(1u<<stationId);
    g.bonusEndsAt[stationId] = 0;
    bcastBonusUpdate(g);
  }

  // Decide hold end: FULL if player filled; else EMPTY (station hit 0)
  if (pl.carried >= g.maxCarry) sendHoldEnd(g, holdId, /*FULL*/0);
  else                          sendHoldEnd(g, holdId, /*EMPTY*/1);
  return true;
}
