#include "ModeClassic.h"
#include "Cadence.h"
#include "Net.h" 
#include "GameAudio.h"
#include "Bonus.h"
#include "Media.h"
#include "esp_system.h"
#include "ServerMini.h"
#include <esp_random.h>
#include <TrexProtocol.h>

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

#include "esp_system.h"  // for esp_random()

static void fillAndShuffleOrder(Game& g, uint8_t avoidFirst /*0 = no guard*/) {
  // Fill 1..MAX_STATIONS
  for (uint8_t i = 0; i < MAX_STATIONS; ++i) g.bonus2Order[i] = i + 1;

  // Fisher–Yates shuffle
  for (int i = MAX_STATIONS - 1; i > 0; --i) {
    uint32_t r = esp_random();
    int j = (int)(r % (uint32_t)(i + 1));
    uint8_t tmp = g.bonus2Order[i];
    g.bonus2Order[i] = g.bonus2Order[j];
    g.bonus2Order[j] = tmp;
  }

  // Ensure we don't immediately repeat the previous SID across cycles
  if (avoidFirst >= 1 && avoidFirst <= MAX_STATIONS && MAX_STATIONS > 1) {
    if (g.bonus2Order[0] == avoidFirst) {
      // swap first two
      uint8_t t = g.bonus2Order[0];
      g.bonus2Order[0] = g.bonus2Order[1];
      g.bonus2Order[1] = t;
    }
  }
  g.bonus2Idx = 0;
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

    sendStateTick(g, (g.roundEndAt > now) ? (g.roundEndAt - now) : 0);

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

  if (idx == 4) {
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
    return;
  }

  if (idx == 5) {
    // ---------- ROUND 5 (dummy = same as ROUND 4) ----------
    g.maxCarry = 10;
    g.noRedThisRound       = false;
    g.allowYellowThisRound = true;
    g.lootRateMs  = 1000;
    g.lootPerTick = 4;

    g.roundStartScore = g.teamScore;
    g.roundGoal       = g.roundStartScore + 40;
    g.roundEndAt      = now + 120000UL;  // or remainder of game if you prefer

    endAndClearHoldsAndCarried(g);

    // RANDOM split of THIS ROUND'S total (same as R4)
    const uint16_t roundTotal = (uint16_t)(g.roundGoal - g.roundStartScore);
    splitInventoryRandom(g, roundTotal);

    const uint32_t redMin  = (g.pirArmDelayMs > 6000) ? g.pirArmDelayMs : 6000;
    g.redMsMin   = redMin;                 g.redMsMax   = (7000 > redMin) ? 7000 : redMin;
    g.greenMsMin = 10000;                  g.greenMsMax = 14000;
    g.yellowMsMin= 3000;                   g.yellowMsMax= 3000;

    g.pending.nextStation = 1;
    g.pending.needScore   = true;

    bonusResetForRound(g, now);
    enterGreen(g);
    bcastRoundStatus(g);
    return;
  }

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
  
  uint32_t now = millis();
  sendStateTick(g, (g.bonusInterEnd > now) ? (g.bonusInterEnd - now) : 0);

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

static uint8_t nextSidSeq(uint8_t cur) {
  uint8_t n;
  do { n = (uint8_t)((esp_random() % ST_LAST) + ST_FIRST); } while (n == cur);
  return n;
}

void startBonusIntermission2(Game& g, uint16_t durationMs /*=15000*/, uint16_t hopMs /*=3000*/) {
  endAndClearHoldsAndCarried(g);

  g.bonusIntermission2 = true;
  g.bonus2Start        = millis();
  g.bonus2End          = g.bonus2Start + durationMs;
  g.bonus2Ms           = durationMs;
  g.bonus2HopMs        = hopMs;
  g.bonusWarnTickStarted = false;

  g.noRedThisRound       = true;
  g.allowYellowThisRound = false;
  enterGreen(g);
  spritePlay(CLIP_LUNCHBREAK);

  // All stations to 0, set TTLs, broadcast
  for (uint8_t sid = 1; sid <= MAX_STATIONS; ++sid) {
    g.stationInventory[sid] = 0;
    g.bonusEndsAt[sid]      = g.bonus2End;
    bcastStation(g, sid);
  }

  // Build first random order (no avoid on the very first cycle)
  fillAndShuffleOrder(g, /*avoidFirst=*/0);

  // Activate the first station in the permutation
  g.bonus2Sid = g.bonus2Order[g.bonus2Idx++];
  g.stationInventory[g.bonus2Sid] = g.stationCapacity[g.bonus2Sid];
  bcastStation(g, g.bonus2Sid);

  g.bonusActiveMask = (1u << g.bonus2Sid);
  bcastBonusUpdate(g);

  g.bonus2NextHopAt = g.bonus2Start + g.bonus2HopMs;

  uint32_t now = millis();
  sendStateTick(g, (g.bonus2End > now) ? (g.bonus2End - now) : 0);
}

void tickBonusIntermission2(Game& g, uint32_t now) {
  if (!g.bonusIntermission2) return;

  // Finish
  if ((int32_t)(now - g.bonus2End) >= 0) {
    for (uint8_t sid = 1; sid <= MAX_STATIONS; ++sid) {
      if (g.stationInventory[sid] != 0) { g.stationInventory[sid] = 0; bcastStation(g, sid); }
      g.bonusEndsAt[sid] = 0;
    }
    g.bonusActiveMask = 0; bcastBonusUpdate(g);
    if (g.bonusWarnTickStarted) { gameAudioStop(); g.bonusWarnTickStarted = false; }

    g.bonusIntermission2 = false;
    g.noRedThisRound       = false;
    g.allowYellowThisRound = true;
    startRound(g, /*idx=*/4);
    return;
  }

  // Per-hop linear decay on active station
  const uint32_t timeLeft = g.bonus2End - now;
  const uint32_t hopEnd   = g.bonus2NextHopAt;
  const uint32_t hopStart = (hopEnd >= g.bonus2HopMs) ? (hopEnd - g.bonus2HopMs) : g.bonus2Start;

  if (now < hopEnd) {
    const uint16_t cap   = g.stationCapacity[g.bonus2Sid];
    const uint32_t el    = (now > hopStart) ? (now - hopStart) : 0;
    const uint32_t hopMs = g.bonus2HopMs ? g.bonus2HopMs : 1;
    const uint16_t target = (el >= hopMs) ? 0 : (uint16_t)((uint64_t)cap * (hopMs - el) / hopMs);
    if (g.stationInventory[g.bonus2Sid] > target) {
      g.stationInventory[g.bonus2Sid] = target;
      bcastStation(g, g.bonus2Sid);
    }
  }

  // Force non-active stations to 0 (and keep them there)
  for (uint8_t sid = 1; sid <= MAX_STATIONS; ++sid) {
    if (sid == g.bonus2Sid) continue;
    if (g.stationInventory[sid] != 0) { g.stationInventory[sid] = 0; bcastStation(g, sid); }
  }

  // Hop?
  if ((int32_t)(now - g.bonus2NextHopAt) >= 0) {
    // Zero current active (if not already)
    if (g.stationInventory[g.bonus2Sid] != 0) {
      g.stationInventory[g.bonus2Sid] = 0;
      bcastStation(g, g.bonus2Sid);
    }

    // If we exhausted the permutation, reshuffle; avoid current SID repeating
    if (g.bonus2Idx >= MAX_STATIONS) {
      fillAndShuffleOrder(g, /*avoidFirst=*/g.bonus2Sid);
    }
    g.bonus2Sid = g.bonus2Order[g.bonus2Idx++];

    // New active to full capacity
    g.stationInventory[g.bonus2Sid] = g.stationCapacity[g.bonus2Sid];
    bcastStation(g, g.bonus2Sid);

    // Update mask & notify for chime at the new station
    g.bonusActiveMask = (1u << g.bonus2Sid);
    bcastBonusUpdate(g);

    // Schedule next hop
    g.bonus2NextHopAt += g.bonus2HopMs;
  }

  // Last-3s tick SFX
  if (!g.bonusWarnTickStarted && timeLeft <= 3000) {
    gameAudioStop();
    gameAudioPlayOnce(TRK_TICKS_LOOP);
    g.bonusWarnTickStarted = true;
  }
}

void modeClassicForceRound(Game& g, uint8_t idx, bool playWin) {
  if (idx < 1) idx = 1;
  if (idx > 5) idx = 5;

  // Make sure we’re in active play
  g.phase = Phase::PLAYING;

  // Optional win sting when jumping forward (kept off for R1 by caller)
  if (playWin) gameAudioPlayOnce(TRK_TREX_WIN);

  // Enter the requested round (startRound already sets timers, inventory,
  // roundStartScore/roundGoal, cadence, and broadcasts ROUND_STATUS)
  startRound(g, idx);
}

// Small local helpers to end intermissions early and clear all bonus state.
static inline void endR25Now(Game& g) {
  // Stop any ticking SFX from last-3s warning if it was armed
  if (g.bonusWarnTickStarted) { gameAudioStop(); g.bonusWarnTickStarted = false; }

  g.bonusIntermission = false;
  g.bonusActiveMask   = 0;
  for (uint8_t sid = 1; sid <= MAX_STATIONS; ++sid) g.bonusEndsAt[sid] = 0;
  bcastBonusUpdate(g);                    // tell Loots to exit bonus visuals

  // restore cadence policy
  g.noRedThisRound       = false;
  g.allowYellowThisRound = true;
}

static inline void endR35Now(Game& g) {
  if (g.bonusWarnTickStarted) { gameAudioStop(); g.bonusWarnTickStarted = false; }

  g.bonusIntermission2 = false;
  g.bonus2Sid          = 0;
  g.bonusActiveMask    = 0;
  for (uint8_t sid = 1; sid <= MAX_STATIONS; ++sid) g.bonusEndsAt[sid] = 0;
  bcastBonusUpdate(g);

  g.noRedThisRound       = false;
  g.allowYellowThisRound = true;
}

// Replace your function with this:
void modeClassicNextRound(Game& g, bool playWin) {
  // If we are currently in Round 2 and not yet in the intermission, go to R2.5.
  if (g.roundIndex == 2 && !g.bonusIntermission) {
    startBonusIntermission(g, /*durationMs=*/15000);
    return;
  }

  // If we're *in* R2.5 and tester presses "next", finish early to R3 and clear bonus.
  if (g.bonusIntermission) {
    endR25Now(g);
    startRound(g, /*idx=*/3);
    return;
  }

  // If we are currently in Round 3 and not yet in the intermission, go to R3.5.
  if (g.roundIndex == 3 && !g.bonusIntermission2) {
    startBonusIntermission2(g, /*durationMs=*/15000, /*hopMs=*/3000);
    return;
  }

  // If we're *in* R3.5 and tester presses "next", finish early to R4 and clear bonus.
  if (g.bonusIntermission2) {
    endR35Now(g);
    startRound(g, /*idx=*/4);
    return;
  }

  // From R4, go to minigame
  if (g.roundIndex == 4) {
    // Arm minigame (top-level fields, consistent with your .ino)
    g.mgActive          = true;
    g.mgCfg.seed        = esp_random();
    g.mgCfg.timerMs     = 60000;   // 1 minute
    g.mgCfg.speedMinMs  = 20;
    g.mgCfg.speedMaxMs  = 80;
    g.mgCfg.segMin      = 6;
    g.mgCfg.segMax      = 16;

    g.mgStartedAt       = millis();
    g.mgDeadline        = g.mgStartedAt + g.mgCfg.timerMs;
    g.mgAllTriedAt      = 0;
    g.mgTriedMask       = 0;
    g.mgSuccessMask     = 0;
    g.mgExpectedStations= 5;       // set to your actual number of Loot stations

    bcastMgStart(g, g.mgCfg);      // broadcast MG_START to clients
    return;                        // IMPORTANT: don't fall through to normal next-round logic yet
  }

  // Otherwise behave like before: advance one round (but never beyond 4).
  uint8_t next = (g.roundIndex >= 5) ? 5 : (g.roundIndex + 1);

  if (g.roundIndex == 0 || g.phase == Phase::END) {
    modeClassicForceRound(g, 1, /*playWin=*/false);
  } else {
    // Safety: if any bonus mask is somehow still set, clear it when skipping.
    if (g.bonusActiveMask) {
      for (uint8_t sid = 1; sid <= MAX_STATIONS; ++sid) g.bonusEndsAt[sid] = 0;
      g.bonusActiveMask = 0;
      g.bonusIntermission  = false;
      g.bonusIntermission2 = false;
      bcastBonusUpdate(g);
      g.noRedThisRound       = false;
      g.allowYellowThisRound = true;
    }
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

  // Do nothing while intermissions are running; their tickers advance them
  if (g.bonusIntermission || g.bonusIntermission2) return;

  // CASE: round goal met
  if (g.teamScore >= g.roundGoal) {
    gameAudioStop();
    if      (g.roundIndex == 1) { startRound(g, 2); return; }
    else if (g.roundIndex == 2) { startBonusIntermission(g, /*durationMs=*/15000); return; }
    else if (g.roundIndex == 3) { startRound(g, 4); return; }
    else if (g.roundIndex == 4) {
      // === START MINIGAME ===
      g.mgActive     = true;
      g.mgCfg.seed   = esp_random();
      g.mgCfg.timerMs= 60000;
      g.mgCfg.speedMinMs = 20;  g.mgCfg.speedMaxMs = 80;
      g.mgCfg.segMin = 6;       g.mgCfg.segMax = 16;
      g.mgStartedAt  = now;
      g.mgDeadline   = now + g.mgCfg.timerMs;
      g.mgAllTriedAt = 0;
      g.mgTriedMask  = 0;
      g.mgSuccessMask= 0;
      g.mgExpectedStations = MAX_STATIONS;    // set 5 if only 5 Loots
      bcastMgStart(g, g.mgCfg);
      // Optional: freeze cadence to GREEN while MG runs
      g.noRedThisRound = true; g.allowYellowThisRound = false;
      return;
    }
  }

  // CASE: round timeout path — do the same for R4 timeout
  if (now >= g.roundEndAt) {
    if (g.teamScore < g.roundGoal) { bcastGameOver(g, /*GOAL_NOT_MET*/4); return; }
    gameAudioStop();
    if      (g.roundIndex == 1) { startRound(g, 2); return; }
    else if (g.roundIndex == 2) { startBonusIntermission(g, /*durationMs=*/15000); return; }
    else if (g.roundIndex == 3) { startRound(g, 4); return; }
    else if (g.roundIndex == 4) {
      // Same MG start as above (timeout also enters MG)
      g.mgActive     = true;
      g.mgCfg.seed   = esp_random();
      g.mgCfg.timerMs= 60000;
      g.mgCfg.speedMinMs = 20;  g.mgCfg.speedMaxMs = 80;
      g.mgCfg.segMin = 6;       g.mgCfg.segMax = 16;
      g.mgStartedAt  = now;
      g.mgDeadline   = now + g.mgCfg.timerMs;
      g.mgAllTriedAt = 0;
      g.mgTriedMask  = 0;
      g.mgSuccessMask= 0;
      g.mgExpectedStations = MAX_STATIONS;
      bcastMgStart(g, g.mgCfg);
      g.noRedThisRound = true; g.allowYellowThisRound = false;
      return;
    }
  }
}
