#include "ModeClassic.h"
#include "Cadence.h"
#include "Net.h" 
#include "GameAudio.h"
#include "Bonus.h"
#include "Media.h"

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

// End any active holds and zero ALL players' carried before a new round
static void endAndClearHoldsAndCarried(Game& g) {
  // End any live holds (clients will clean up visuals/audio on HOLD_END)
  for (uint8_t i = 0; i < MAX_HOLDS; ++i) {
    if (g.holds[i].active) {
      sendHoldEnd(g, g.holds[i].holdId, /*EMPTY*/1);
      g.holds[i].active = false;
    }
  }
  // Zero every player's carried so nothing rolls into the next round
  for (uint8_t pi = 0; pi < MAX_PLAYERS; ++pi) {
    g.players[pi].carried = 0;
  }
}

static void startRound(Game& g, uint8_t idx) {
  const uint32_t now = millis();
  g.roundIndex   = idx;
  g.roundStartAt = now;

  gameAudioStop();
  if (idx > 1) gameAudioPlayOnce(TRK_TREX_WIN);

  if (idx == 1) {
    // ---------- ROUND 1 ----------
    g.maxCarry = 20;
    g.noRedThisRound       = true;
    g.allowYellowThisRound = false;

    g.gameStartAt = now;
    g.gameEndAt   = now + 300000UL;  // 5:00 total
    g.roundEndAt  = now + 120000UL;  // 2:00

    g.roundStartScore = 0;
    g.roundGoal       = 40;
    g.lootPerTick     = 4;
    g.lootRateMs      = 1000;

    endAndClearHoldsAndCarried(g);

    // EVEN split of THIS ROUND'S total (goal - start)
    const uint16_t roundTotal = (uint16_t)(g.roundGoal - g.roundStartScore); // =40
    uint16_t base = roundTotal / 5, rem = roundTotal % 5;
    for (uint8_t sid = 1; sid <= 5; ++sid) {
      uint16_t x = base + (rem ? 1 : 0); if (rem) rem--;
      if (x > 56) x = 56;
      g.stationCapacity[sid]  = 56;
      g.stationInventory[sid] = x;
    }

    g.pending.needGameStart = true;
    g.pending.nextStation   = 1;
    g.pending.needScore     = true;

    bonusResetForRound(g, now);
    enterGreen(g);
    bcastRoundStatus(g);
    return;
  }

  if (idx == 2) {
    // ---------- ROUND 2 ----------
    g.maxCarry = 20;
    g.noRedThisRound       = false;
    g.allowYellowThisRound = true;
    g.lootRateMs  = 1000;
    g.lootPerTick = 4;

    g.roundStartScore = g.teamScore;
    g.roundGoal       = g.roundStartScore + 40;
    g.roundEndAt      = now + 120000UL;

    endAndClearHoldsAndCarried(g);

    // EVEN split of THIS ROUND'S total
    const uint16_t roundTotal = (uint16_t)(g.roundGoal - g.roundStartScore);
    uint16_t base = roundTotal / 5, rem = roundTotal % 5;
    for (uint8_t sid = 1; sid <= 5; ++sid) {
      uint16_t x = base + (rem ? 1 : 0); if (rem) rem--;
      if (x > 56) x = 56;
      g.stationCapacity[sid]  = 56;
      g.stationInventory[sid] = x;
    }

    g.pending.nextStation = 1;
    g.pending.needScore   = true;

    bonusResetForRound(g, now);
    enterGreen(g);
    bcastRoundStatus(g);
    return;
  }

  if (idx == 3) {
    // ---------- ROUND 3 ----------
    g.maxCarry = 10;
    g.noRedThisRound       = false;
    g.allowYellowThisRound = true;
    g.lootRateMs  = 1000;
    g.lootPerTick = 4;

    g.roundStartScore = g.teamScore;
    g.roundGoal       = g.roundStartScore + 40;
    g.roundEndAt      = now + 120000UL;

    endAndClearHoldsAndCarried(g);

    // RANDOM split of THIS ROUND'S total
    const uint16_t roundTotal = (uint16_t)(g.roundGoal - g.roundStartScore);
    splitInventoryRandom(g, roundTotal);

    // Your R3 cadence feel
    g.greenMsMin = 14000;  g.greenMsMax = 18000;
    g.redMsMin   = 6500;   g.redMsMax   = 8000;
    g.yellowMsMin= g.yellowMs; g.yellowMsMax = g.yellowMs;

    g.pending.nextStation = 1;
    g.pending.needScore   = true;

    bonusResetForRound(g, now);
    enterGreen(g);
    bcastRoundStatus(g);
    return;
  }

  // ---------- ROUND 4 ----------
  g.maxCarry = 10;
  g.noRedThisRound       = false;
  g.allowYellowThisRound = true;
  g.lootRateMs  = 1000;
  g.lootPerTick = 4;

  g.roundStartScore = g.teamScore;
  g.roundGoal       = g.roundStartScore + 40;
  g.roundEndAt      = (g.gameEndAt > now) ? g.gameEndAt : now; // remainder

  endAndClearHoldsAndCarried(g);

  // RANDOM split of THIS ROUND'S total
  {
    const uint16_t roundTotal = (uint16_t)(g.roundGoal - g.roundStartScore);
    splitInventoryRandom(g, roundTotal);
  }

  const uint32_t redMin  = (g.pirArmDelayMs > 6000) ? g.pirArmDelayMs : 6000;
  g.redMsMin   = redMin;                 g.redMsMax   = (7000 > redMin) ? 7000 : redMin;
  g.greenMsMin = 10000;                  g.greenMsMax = 14000;
  g.yellowMsMin= 3000;                   g.yellowMsMax = 3000;

  g.pending.nextStation = 1;
  g.pending.needScore   = true;

  bonusResetForRound(g, now);
  enterGreen(g);
  bcastRoundStatus(g);
}

static const uint8_t ST_FIRST = 1;
static const uint8_t ST_LAST  = MAX_STATIONS;

void startBonusIntermission(Game& g, uint16_t durationMs /*=15000*/) {
  // End any holds and clear carried so nothing rolls into intermission
  endAndClearHoldsAndCarried(g);

  // Mark intermission window
  g.bonusIntermission = true;
  g.bonusInterMs      = durationMs;
  g.bonusInterStart   = millis();
  g.bonusInterEnd     = g.bonusInterStart + durationMs;
  g.bonusWarnTickStarted = false;   // arm the last-3s tick

  // Lock cadence to GREEN (no yellow/red during intermission)
  g.noRedThisRound       = true;
  g.allowYellowThisRound = false;
  enterGreen(g);

  // NEW: play the intermission video on the Sprite
  spritePlay(CLIP_LUNCHBREAK);

  // Fill every station to capacity, broadcast, and mark bonus ON for all
  g.bonusActiveMask = 0;
  for (uint8_t sid = ST_FIRST; sid <= ST_LAST; ++sid) {
    g.stationInventory[sid] = g.stationCapacity[sid];
    bcastStation(g, sid);
    g.bonusActiveMask |= (1u << sid);
    g.bonusEndsAt[sid] = g.bonusInterEnd;
  }
  bcastBonusUpdate(g);  // clients: rainbow + spawn chime
}

void tickBonusIntermission(Game& g, uint32_t now) {
  if (!g.bonusIntermission) return;

  // Finish condition
  if ((int32_t)(now - g.bonusInterEnd) >= 0) {
    for (uint8_t sid = ST_FIRST; sid <= ST_LAST; ++sid) {
      if (g.stationInventory[sid] != 0) {
        g.stationInventory[sid] = 0;
        bcastStation(g, sid);
      }
      g.bonusEndsAt[sid] = 0;
    }
    g.bonusActiveMask   = 0;
    bcastBonusUpdate(g);

    g.bonusIntermission = false;

    // Restore cadence policy and start Round 3
    g.noRedThisRound       = false;
    g.allowYellowThisRound = true;

    if (g.bonusWarnTickStarted) {
      gameAudioStop();                    // stop the tick when intermission ends
      g.bonusWarnTickStarted = false;
    }
    
    startRound(g, /*idx=*/3);
    return;
  }

  // Linear auto-decay toward 0 by end-of-window
  const uint32_t T        = g.bonusInterEnd - g.bonusInterStart;
  const uint32_t timeLeft = g.bonusInterEnd - now;

  // Start tick SFX once at T<=3s
  if (!g.bonusWarnTickStarted && timeLeft <= 3000) {
    gameAudioStop();
    gameAudioPlayOnce(TRK_TICKS_LOOP);   // same tick you use for yellow
    g.bonusWarnTickStarted = true;
  }

  for (uint8_t sid = ST_FIRST; sid <= ST_LAST; ++sid) {
    const uint16_t cap    = g.stationCapacity[sid];
    const uint16_t target = (uint16_t)((uint64_t)cap * timeLeft / T);
    if (g.stationInventory[sid] > target) {
      g.stationInventory[sid] = target;
      bcastStation(g, sid);
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
  // If we are currently in Round 2 and not yet in the intermission, go to R2.5.
  if (g.roundIndex == 2 && !g.bonusIntermission) {
    startBonusIntermission(g, /*durationMs=*/15000);  // 15s
    return;
  }

  // If we're *in* the intermission already and tester presses "next", finish early to R3.
  if (g.bonusIntermission) {
    startRound(g, /*idx=*/3);  // same behavior as natural end
    return;
  }

  // Otherwise behave like before.
  uint8_t next = (g.roundIndex >= 4) ? 4 : (g.roundIndex + 1);
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

  // NEW: During the 2.5 intermission, advancement is driven by tickBonusIntermission()
  if (g.bonusIntermission) return;

  // 1) Early advance on goal
  if (g.teamScore >= g.roundGoal) {
    gameAudioStop();
    if      (g.roundIndex == 1) { startRound(g, 2); return; }
    else if (g.roundIndex == 2) { startBonusIntermission(g, /*durationMs=*/15000); return; }
    else if (g.roundIndex == 3) { startRound(g, 4); return; }
  }

  // 2) Round timeout: must meet goal, else game over; on success, advance
  if (now >= g.roundEndAt) {
    if (g.teamScore < g.roundGoal) { bcastGameOver(g, /*GOAL_NOT_MET*/4); return; }
    gameAudioStop();
    if      (g.roundIndex == 1) { startRound(g, 2); return; }
    else if (g.roundIndex == 2) { startBonusIntermission(g, /*durationMs=*/15000); return; }
    else if (g.roundIndex == 3) { startRound(g, 4); return; }
  }
}
