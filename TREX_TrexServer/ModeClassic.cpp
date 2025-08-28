#include "ModeClassic.h"
#include "Cadence.h"
#include "Net.h" 
#include "GameAudio.h"

// --- Random split of TOTAL across 5 stations, each <= 56 ---
static void splitInventoryRandom(Game& g, uint16_t total /*=100*/) {
  uint16_t remain = total;
  for (uint8_t sid = 1; sid <= 5; ++sid) {
    const uint8_t left = (uint8_t)(5 - sid + 1);
    const uint16_t maxPer = 56;
    const uint16_t minX = (remain > (left - 1)*maxPer) ? (uint16_t)(remain - (left - 1)*maxPer) : 0;
    const uint16_t maxX = (remain < maxPer) ? remain : maxPer;
    uint16_t x = (sid < 5)
      ? (uint16_t)(minX + (esp_random() % (uint32_t)(maxX - minX + 1)))
      : remain; // last takes the rest
    g.stationCapacity[sid]  = maxPer;
    g.stationInventory[sid] = x;
    remain -= x;
  }
}

static void startRound(Game& g, uint8_t idx) {
  const uint32_t now = millis();
  g.roundIndex   = idx;
  g.roundStartAt = now;

  gameAudioStop();
  if (idx > 1) gameAudioPlayOnce(TRK_TREX_WIN);

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
    } else if (idx == 3) {
      // ---------- ROUND 3 ----------
      g.maxCarry = 12;
      g.noRedThisRound       = false;
      g.allowYellowThisRound = true;
      g.lootRateMs  = 1000;
      g.lootPerTick = 4;

      // New 2-minute round with +100 from this point
      g.roundStartScore = g.teamScore;
      g.roundGoal       = g.roundStartScore + 100;   // absolute threshold
      g.roundEndAt      = now + 120000UL;            // 2:00

      // Randomly re-split total loot across stations
      splitInventoryRandom(g, /*total=*/100);
      g.pending.nextStation = 1;   // drip stations again
      g.pending.needScore   = true;

      // Randomized cadence dwell ranges for GREEN/RED.
      // (Adjust these ranges as you like; YELLOW kept fixed but range fields set equal for future proofing)
      g.greenMsMin = 7000;  g.greenMsMax = 13000;     // ~7–13 s
      g.redMsMin   = 6000;  g.redMsMax   = 11000;     // ~6–11 s
      g.yellowMsMin= g.yellowMs; g.yellowMsMax = g.yellowMs;  // no randomization (yet)

      enterGreen(g);
      bcastRoundStatus(g);
    } else { // idx >= 4
      // ---------- ROUND 4 (placeholder) ----------
      // Play out the remainder of the game with current settings.
      g.roundEndAt = (g.gameEndAt > now) ? g.gameEndAt : now;
      enterGreen(g);
      // (If you want R4 to have its own goal/timer, say the word and we’ll wire it like R2/R3.)
    }
  }
}

void modeClassicForceRound(Game& g, uint8_t idx, bool playWin) {
  if (idx < 1) idx = 1;
  if (idx > 4) idx = 4;

  // Make sure we’re in active play
  g.phase = Phase::PLAYING;

  // Optional win sting when jumping forward (kept off for R1 by caller)
  if (playWin) gameAudioPlayOnce(TRK_TREX_WIN);

  // Enter the requested round (startRound already sets timers, inventory,
  // roundStartScore/roundGoal, cadence, and broadcasts ROUND_STATUS)
  startRound(g, idx);
}

void modeClassicNextRound(Game& g, bool playWin) {
  uint8_t next = (g.roundIndex >= 4) ? 4 : (g.roundIndex + 1);
  // For next from R0/END, treat as R1 without sting:
  if (g.roundIndex == 0 || g.phase == Phase::END) {
    modeClassicForceRound(g, 1, /*playWin=*/false);
  } else {
    modeClassicForceRound(g, next, playWin);
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

  // 0) Global expiry wins over everything
  if (now >= g.gameEndAt) { bcastGameOver(g, /*TIME_UP*/0); return; }

  // 1) Early advance on goal
  if (g.teamScore >= g.roundGoal) {
    gameAudioStop();
    if      (g.roundIndex == 1) { startRound(g, 2); return; }
    else if (g.roundIndex == 2) { startRound(g, 3); return; }
    else if (g.roundIndex == 3) { startRound(g, 4); return; }
  }

  // 2) Round timeout: must meet goal, else game over; on success, advance
  if (now >= g.roundEndAt) {
    if (g.teamScore < g.roundGoal) { bcastGameOver(g, /*GOAL_NOT_MET*/4); return; }
    gameAudioStop();
    if      (g.roundIndex == 1) { startRound(g, 2); return; }
    else if (g.roundIndex == 2) { startRound(g, 3); return; }
    else if (g.roundIndex == 3) { startRound(g, 4); return; }
  }
}
