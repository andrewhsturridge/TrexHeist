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

// -------- R5 internals (file-local) --------
static inline uint32_t rr() { return esp_random(); }

static void r5Shuffle(uint8_t a[5]) {
  for (int i=4;i>0;--i) {
    int j = rr() % (i+1);
    uint8_t t=a[i]; a[i]=a[j]; a[j]=t;
  }
}

static bool r5AnyHoldOnSid(const Game& g, uint8_t sid) {
  for (const auto &h : g.holds) if (h.active && h.stationId == sid) return true;
  return false;
}

static void r5SetHot(Game &g, uint8_t sid, uint32_t now) {
  // For now keep Round 5 in GREEN only
  enterGreen(g);

  // One-hot inventory: hot=100%, others=0
  for (uint8_t s=1; s<=5; ++s) {
    uint16_t inv = (s==sid) ? g.stationCapacity[s] : 0;
    if (g.stationInventory[s] != inv) {
      g.stationInventory[s] = inv;
      bcastStation(g, s);
    }
  }

  g.r5HotSid = sid;

  // Random dwell within window
  uint16_t span = (g.r5DwellMaxMs > g.r5DwellMinMs) ? (g.r5DwellMaxMs - g.r5DwellMinMs) : 0;
  uint16_t dwell = g.r5DwellMinMs + (span ? (rr() % (span+1)) : 0);
  g.r5DwellEndAt    = now + dwell;
  g.r5NextDepleteAt = now + g.r5DepleteStepMs;
}

static void r5Start(Game &g, uint32_t now) {
  if (g.r5Active) return;
  g.r5Active = true;

  // Start with a shuffle-bag cycle
  g.r5Order[0]=1; g.r5Order[1]=2; g.r5Order[2]=3; g.r5Order[3]=4; g.r5Order[4]=5;
  r5Shuffle(g.r5Order);
  g.r5Idx = 0;

  // Lock cadence policy (future-friendly knobs)
  g.noRedThisRound = true;
  g.allowYellowThisRound = false;

  r5SetHot(g, g.r5Order[g.r5Idx], now);
}

static void r5HopNext(Game &g, uint32_t now) {
  if (++g.r5Idx >= 5) { r5Shuffle(g.r5Order); g.r5Idx = 0; }
  r5SetHot(g, g.r5Order[g.r5Idx], now);
}

// Call only when roundIndex==5 & PLAYING
static void r5Tick(Game &g, uint32_t now) {
  // Dwell expiry → hop
  if ((int32_t)(now - g.r5DwellEndAt) >= 0) {
    r5HopNext(g, now);
  }

  // Idle deplete hot station (no active hold)
  const uint8_t sid = g.r5HotSid;
  if (!sid) return;

  if (!r5AnyHoldOnSid(g, sid) &&
      g.stationInventory[sid] > 0 &&
      (int32_t)(now - g.r5NextDepleteAt) >= 0) {

    uint16_t inv = g.stationInventory[sid];
    uint16_t dec = g.r5DepletePerStep;
    if (dec > inv) dec = inv;
    g.stationInventory[sid] = (uint16_t)(inv - dec);
    bcastStation(g, sid);
    g.r5NextDepleteAt = now + g.r5DepleteStepMs;
  }
}

static void startRound(Game& g, uint8_t idx) {
  const uint32_t now = millis();
  g.roundIndex   = idx;
  g.roundStartAt = now;

  // Reset any previous R5 state when (re)starting a round; r5Start() will arm it for idx==5.
  g.r5Active        = false;
  g.r5HotSid        = 0;
  g.r5DwellEndAt    = 0;
  g.r5NextDepleteAt = 0;

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
    g.roundEndAt      = now + 120000UL;

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
    // ---------- ROUND 5 ----------
    // Keep GREEN; no bonus in this round (for now).
    g.noRedThisRound       = true;
    g.allowYellowThisRound = false;

    // Same carry/loot cadence as Round 4
    g.maxCarry    = 10;
    g.lootRateMs  = 1000;
    g.lootPerTick = 4;

    // Same goal & timing model as Round 4
    g.roundStartScore = g.teamScore;
    g.roundGoal       = g.roundStartScore + 40;
    g.roundEndAt      = now + 120000UL;

    // Broadcast timers like other rounds
    sendStateTick(g, (g.roundEndAt > now) ? (g.roundEndAt - now) : 0);

    // Clear any rolls from previous round
    endAndClearHoldsAndCarried(g);

    // Enter GREEN and tell clients
    enterGreen(g);
    bcastRoundStatus(g);

    // *** Start the R5 hop engine ***
    r5Start(g, now);
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

void modeClassicNextRound(Game& g, bool playWin) {
  // Finish R2 -> R2.5 path
  if (g.roundIndex == 2 && !g.bonusIntermission) {
    startBonusIntermission(g, /*durationMs=*/15000);
    return;
  }

  // If *in* R2.5 and tester presses "next", finish early to R3 and clear bonus.
  if (g.bonusIntermission) {
    if (g.bonusWarnTickStarted) { gameAudioStop(); g.bonusWarnTickStarted = false; }
    g.bonusIntermission = false;
    g.bonusActiveMask   = 0;
    for (uint8_t sid = 1; sid <= MAX_STATIONS; ++sid) g.bonusEndsAt[sid] = 0;
    bcastBonusUpdate(g);
    g.noRedThisRound       = false;
    g.allowYellowThisRound = true;
    startRound(g, /*idx=*/3);
    return;
  }

  // Finish R3 -> R3.5 path
  if (g.roundIndex == 3 && !g.bonusIntermission2) {
    startBonusIntermission2(g, /*durationMs=*/15000, /*hopMs=*/3000);
    return;
  }

  // If *in* R3.5 and tester presses "next", finish early to R4 and clear bonus.
  if (g.bonusIntermission2) {
    if (g.bonusWarnTickStarted) { gameAudioStop(); g.bonusWarnTickStarted = false; }
    g.bonusIntermission2 = false;
    g.bonus2Sid          = 0;
    g.bonusActiveMask    = 0;
    for (uint8_t sid = 1; sid <= MAX_STATIONS; ++sid) g.bonusEndsAt[sid] = 0;
    bcastBonusUpdate(g);
    g.noRedThisRound       = false;
    g.allowYellowThisRound = true;
    startRound(g, /*idx=*/4);
    return;
  }

  // If MG is currently running and tester presses "next", cancel MG and jump to Round 5.
  if (g.mgActive) {
    g.mgActive = false;
    bcastMgStop(g);
    modeClassicForceRound(g, 5, /*playWin=*/false);  // startRound(5) will arm R5 engine
    return;
  }

  // From R4, pressing "next" starts the minigame (if not already running).
  if (g.roundIndex == 4) {
    g.mgActive     = true;
    g.mgCfg.seed   = esp_random();
    g.mgCfg.timerMs= 60000;
    g.mgCfg.speedMinMs = 20;  g.mgCfg.speedMaxMs = 80;
    g.mgCfg.segMin = 6;       g.mgCfg.segMax = 16;
    g.mgStartedAt  = millis();
    g.mgDeadline   = g.mgStartedAt + g.mgCfg.timerMs;
    g.mgAllTriedAt = 0;
    g.mgTriedMask  = 0;
    g.mgSuccessMask= 0;
    g.mgExpectedStations = MAX_STATIONS;    // set 5 if only 5 Loots
    bcastMgStart(g, g.mgCfg);
    g.noRedThisRound = true; g.allowYellowThisRound = false;
    return;
  }

  // Otherwise: advance one round (cap at 5). startRound(5) will start the R5 hop engine.
  uint8_t next = (g.roundIndex >= 5) ? 5 : (g.roundIndex + 1);

  // Make sure we’re in active play and optionally play win sting
  g.phase = Phase::PLAYING;
  if (playWin && next > 1) gameAudioPlayOnce(TRK_TREX_WIN);

  // Safety: if any bonus mask is still set, clear it when skipping.
  if (g.bonusActiveMask) {
    for (uint8_t sid = 1; sid <= MAX_STATIONS; ++sid) g.bonusEndsAt[sid] = 0;
    g.bonusActiveMask = 0;
    g.bonusIntermission  = false;
    g.bonusIntermission2 = false;
    bcastBonusUpdate(g);
    g.noRedThisRound       = false;
    g.allowYellowThisRound = true;
  }

  startRound(g, next);
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

  // ===== Goal met path =====
  if (g.teamScore >= g.roundGoal) {
    gameAudioStop();

    if      (g.roundIndex == 1) { startRound(g, 2); return; }
    else if (g.roundIndex == 2) { startBonusIntermission(g, /*durationMs=*/15000); return; }
    else if (g.roundIndex == 3) { startRound(g, 4); return; }
    else if (g.roundIndex == 4) {
      // === START MINIGAME (between R4->R5) ===
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
      g.noRedThisRound = true; g.allowYellowThisRound = false;
      return;
    }
    else if (g.roundIndex == 5) {
      // R5 goal reached -> end game (success)
      bcastGameOver(g, /*GOAL_MET*/0, GAMEOVER_BLAME_ALL);
      return;
    }
  }

  // *** Guard: if no round end time is set yet, do not treat it as a timeout ***
  if (g.roundEndAt == 0) {
    return;
  }

  // ===== Timeout path =====
  if (now >= g.roundEndAt) {
    // If the team didn't meet the goal by timeout -> failure
    if (g.teamScore < g.roundGoal) {
      bcastGameOver(g, /*GOAL_NOT_MET*/4, GAMEOVER_BLAME_ALL);
      return;
    }

    gameAudioStop();

    if      (g.roundIndex == 1) { startRound(g, 2); return; }
    else if (g.roundIndex == 2) { startBonusIntermission(g, /*durationMs=*/15000); return; }
    else if (g.roundIndex == 3) { startRound(g, 4); return; }
    else if (g.roundIndex == 4) {
      // Timeout at R4 also enters the minigame
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
    else if (g.roundIndex == 5) {
      // R5 timeout -> success if goal was met, otherwise failure
      if (g.teamScore >= g.roundGoal) {
        bcastGameOver(g, /*GOAL_MET*/0, GAMEOVER_BLAME_ALL);
      } else {
        bcastGameOver(g, /*GOAL_NOT_MET*/4, GAMEOVER_BLAME_ALL);
      }
      return;
    }
  }
}

void modeClassicOnPlayingTick(Game& g, uint32_t now) {
  // Other per-frame mechanics while PLAYING (e.g., Round 5 hop/deplete)
  if (g.roundIndex == 5 && g.r5Active) {
    r5Tick(g, now);
  }
}
