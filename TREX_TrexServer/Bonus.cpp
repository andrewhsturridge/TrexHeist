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

static void drainActiveHoldsOnStation(Game& g, uint8_t sid) {
  for (uint8_t i = 0; i < MAX_HOLDS; ++i) {
    if (g.holds[i].active && g.holds[i].stationId == sid) {
      if (applyBonusOnHoldStart(g, g.holds[i].playerIdx, sid, g.holds[i].holdId)) {
        g.holds[i].active = false;   // ended FULL/EMPTY by the drain
      }
    }
  }
}

// When a station enters BONUS while being looted, stop the current hold
// but DON'T drain here; player must remove + re-tap to vacuum on hold start.
static void endActiveHoldsOnStation(Game& g, uint8_t sid) {
  for (uint8_t i = 0; i < MAX_HOLDS; ++i) {
    if (g.holds[i].active && g.holds[i].stationId == sid) {
      sendHoldEnd(g, g.holds[i].holdId, /*INTERRUPT*/2);  // reason value is arbitrary; clients ignore
      g.holds[i].active = false;
    }
  }
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
  if (obeyCap) {
    uint32_t m = g.bonusActiveMask; uint8_t active=0; while (m){ m&=(m-1); ++active; }
    if (active >= p.maxConcurrent) return;
  }

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
      endActiveHoldsOnStation(g, sid);   // end current hold; wait for re-tap to vacuum
    }
  } else {
    const uint8_t sid = elig[(uint8_t)random(0, eCount)];
    g.bonusActiveMask |= (1u<<sid);
    g.bonusEndsAt[sid] = now + p.durationMs;
    endActiveHoldsOnStation(g, sid);
  }

  ++g.bonusSpawnsThisRound;
  bcastBonusUpdate(g);                   // clients play chime + rainbow
}

void tickBonusDirector(Game& g, uint32_t now) {
  // Expire finished or empty immediately
  bool dirty = false;
  for (uint8_t sid = 1; sid <= MAX_STATIONS; ++sid) {
    if (g.bonusActiveMask & (1u << sid)) {
      const bool ttlOver = (g.bonusEndsAt[sid] > 0) && (now >= g.bonusEndsAt[sid]);
      const bool empty   = (g.stationInventory[sid] == 0);
      if (ttlOver || empty) {
        g.bonusActiveMask &= ~(1u << sid);
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

  // NEW: If we're not GREEN, defer the spawn until we are.
  if (g.light == LightState::RED) {
    // Do NOT reschedule; keeping nextSpawnAt in the past guarantees
    // we will spawn immediately on the first GREEN tick.
    return;
  }

  // GREEN (or YELLOW) → proceed
  spawnNow(g, now, p, /*obeyCap=*/true);
  g.bonusNextSpawnAt = now + jittered(p.intervalMeanMs, p.intervalJitterMs);
}

void bonusForceSpawn(Game& g, uint32_t now) {
  if (!(g.roundIndex == 3 || g.roundIndex == 4)) {
    Serial.println("[BONUS] force ignored (not R3/R4)");
    return;
  }

  // NEW: Defer if RED; will auto-fire on first GREEN via tickBonusDirector
  if (g.light == LightState::RED) {
    Serial.println("[BONUS] force deferred (RED)");
    // Ensure the scheduler sees it as "due" the moment we turn GREEN
    if (g.bonusNextSpawnAt == 0 || g.bonusNextSpawnAt > now) g.bonusNextSpawnAt = now;
    return;
  }

  Serial.println("[BONUS] Starting");
  BonusParams p = paramsForRound(g.roundIndex);
  spawnNow(g, now, p, /*obeyCap=*/true);
  // (do not advance counters/timers for manual; keep current behavior)
}

void bonusClearAll(Game& g) {
  g.bonusActiveMask = 0;
  for (int i=1;i<=MAX_STATIONS;++i) g.bonusEndsAt[i] = 0;
  bcastBonusUpdate(g);
}

// Apply "bonus vacuum": empty the station and load EVERYTHING into player's carried,
// even if it exceeds maxCarry. Returns true if we ended the hold here.
bool applyBonusOnHoldStart(Game& g, uint8_t playerIdx, uint8_t stationId, uint32_t holdId) {
  // Only if this station is currently bonus and has inventory
  if ((g.bonusActiveMask & (1u << stationId)) == 0) return false;

  uint32_t inv = g.stationInventory[stationId];
  if (inv == 0) return false;

  auto &pl = g.players[playerIdx];

  // Take ALL remaining inventory from the station (no cap), add to carried
  uint32_t beforeCarry = pl.carried;
  pl.carried += inv;                  // can exceed g.maxCarry by design
  g.stationInventory[stationId] = 0;  // station is now empty

  // Notify everyone (tick first so client sees FULL ring before HOLD_END)
  sendLootTick(g, holdId, pl.carried, /*inventory*/0);
  bcastStation(g, stationId);

  // Since station is empty, clear its bonus flag immediately
  g.bonusActiveMask &= ~(1u << stationId);
  g.bonusEndsAt[stationId] = 0;
  bcastBonusUpdate(g);

  // End this hold right away. We can mark FULL (client ignores reason, but matches UX).
  sendHoldEnd(g, holdId, /*FULL*/0);

  // Debug: trace how much we moved
  Serial.printf("[BONUS VACUUM] sid=%u took=%lu carry %lu->%lu\n",
                stationId, (unsigned long)inv,
                (unsigned long)beforeCarry, (unsigned long)pl.carried);

  return true;
}
