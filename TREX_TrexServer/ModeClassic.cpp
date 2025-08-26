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
  } else {
    // Round 2: enable full cadence until game end
    g.maxCarry = 8;
    g.noRedThisRound       = false;
    g.allowYellowThisRound = true;
    g.lootRateMs  = 1000;
    g.lootPerTick = 2;   


    g.roundEndAt = (g.gameEndAt > now) ? g.gameEndAt : now;  // rest of game
    enterGreen(g);
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

  // Early advance: if Round 1 goal reached, go to Round 2 immediately
  if (g.roundIndex == 1 && g.teamScore >= g.roundGoal) {
    startRound(g, /*idx=*/2);
    return;
  }
  // Round timing
  if (now >= g.gameEndAt) {
    bcastGameOver(g, /*TIME_UP*/0);
    return;
  }
  // Round 1 timeout
  if (g.roundIndex == 1 && now >= g.roundEndAt) {
    if (g.teamScore < g.roundGoal) {
      bcastGameOver(g, /*GOAL_NOT_MET*/4);
      return;
    }
    startRound(g, /*idx=*/2);   // success -> play out remainder of game
  }
}
