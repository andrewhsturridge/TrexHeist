#include "ModeClassic.h"
#include "Cadence.h"
#include "Net.h" 

static void startRound(Game& g, uint8_t idx) {
  const uint32_t now = millis();
  g.roundIndex   = idx;
  g.roundStartAt = now;

  if (idx == 1) {
    g.maxCarry = 20;
    g.noRedThisRound       = true;
    g.allowYellowThisRound = false;

    g.gameStartAt = now;
    g.gameEndAt   = now + 300000UL;  // 5:00 total
    g.roundEndAt  = now + 120000UL;  // 2:00 Round 1
    g.roundStartScore = 0;

    g.roundGoal   = 100;
    g.lootPerTick  = 4;
    g.lootRateMs  = 1000;

    // initialize stations (no broadcast here; use drip)
    for (uint8_t sid = 1; sid <= 5; ++sid) {
      g.stationCapacity[sid]  = 56;
      g.stationInventory[sid] = 20;
    }

    // prime drip
    g.pending.needGameStart = true;
    g.pending.nextStation   = 1;
    g.pending.needScore     = true;

    enterGreen(g);  // lock GREEN in Round 1
    bcastRoundStatus(g);
  } else {
    if (idx == 2) {
      // Round 2 (new 2:00 goal round)
      g.maxCarry = 20;
      g.noRedThisRound       = false;   // full cadence enabled
      g.allowYellowThisRound = true;
      g.lootRateMs  = 1000;
      g.lootPerTick = 4;

      // Two-minute round starting now
      g.roundStartScore = g.teamScore;
      g.roundGoal       = g.roundStartScore + 100;   // +100 from this point
      g.roundEndAt      = now + 120000UL;            // 2:00

      // Evenly re-split station inventory (mirror R1 setup)
      for (uint8_t sid = 1; sid <= 5; ++sid) {
        g.stationCapacity[sid]  = 56;
        g.stationInventory[sid] = 20;
      }
      // Prime drip to rebroadcast stations/score
      g.pending.nextStation = 1;
      g.pending.needScore   = true;

      enterGreen(g);
      bcastRoundStatus(g);
    } else {
      // Round 3: play out remainder of game (you can tune later)
      g.maxCarry = 8;
      g.noRedThisRound       = false;
      g.allowYellowThisRound = true;
      g.lootRateMs  = 1000;
      g.lootPerTick = 2;

      g.roundEndAt = (g.gameEndAt > now) ? g.gameEndAt : now;
      enterGreen(g);
    }
  }
}

void modeClassicInit(Game& g) {
  const uint32_t now = millis();
  // overall 5-minute game
  g.gameStartAt = now;
  g.gameEndAt   = now + 300000UL;           // 5 minutes

  startRound(g, /*idx=*/1);                 // Round 1 starts immediately
}

void modeClassicMaybeAdvance(Game& g) {
  const uint32_t now = millis();

  // Round timing
  if (now >= g.gameEndAt) {
    bcastGameOver(g, /*TIME_UP*/0);
    return;
  }
  if (g.roundIndex == 1) {
    // Early advance: hit R1 goal -> Round 2
    if (g.teamScore >= g.roundGoal) {
      startRound(g, /*idx=*/2);
      return;
    }
    // R1 timeout: must meet goal, else game over
    if (now >= g.roundEndAt) {
      if (g.teamScore < g.roundGoal) { bcastGameOver(g, /*GOAL_NOT_MET*/4); return; }
      startRound(g, /*idx=*/2);
      return;
    }
  } else if (g.roundIndex == 2) {
    // R2 success -> Round 3
    if (g.teamScore >= g.roundGoal) {
      startRound(g, /*idx=*/3);
      return;
    }
    // R2 timeout: must meet goal, else game over
    if (now >= g.roundEndAt) {
      if (g.teamScore < g.roundGoal) { bcastGameOver(g, /*GOAL_NOT_MET*/4); return; }
      startRound(g, /*idx=*/3);
      return;
    }
  }
}
