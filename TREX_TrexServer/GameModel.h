#pragma once
#include <Arduino.h>
#include <TrexProtocol.h>
#include "ServerMini.h"

constexpr uint8_t MAX_PLAYERS = 24;
constexpr uint8_t MAX_HOLDS   = 8;
constexpr uint8_t MAX_STATIONS= 5;

// Phases
enum class Phase : uint8_t { PLAYING=1, END=2 };

struct PlayerRec {
  TrexUid  uid{};
  bool     used=false;
  uint8_t  carried=0;
  uint32_t banked=0;
};

struct HoldRec {
  bool     active=false;
  uint32_t holdId=0;
  uint8_t  stationId=0;
  uint8_t  playerIdx=255;
  uint32_t nextTickAt=0;
};

struct PirRec {
  int8_t   pin=-1;
  bool     state=false;
  bool     last=false;
  uint32_t lastChange=0;
};

struct PendingStart {
  bool    needGameStart = false;
  uint8_t nextStation   = 0;  // 1..5
  bool    needScore     = false;
};

struct Game {
  // Core
  Phase       phase      = Phase::PLAYING;
  LightState  light      = LightState::GREEN;
  uint32_t    nextSwitch = 0;
  uint16_t    seq        = 1;
  uint32_t    teamScore  = 0;

  // Tunables (Telnet/maint will edit these)
  uint32_t greenMs = 15000;
  uint32_t redMs   = 6500;
  uint32_t yellowMs = 3000;
  uint32_t lootRateMs = 1000;
  uint16_t lootPerTick = 1;
  uint8_t  maxCarry = 8;
  uint8_t  tickHz   = 5;
  bool     redEnabled = true;
  bool allowYellowThisRound = true;

  uint32_t greenMsMin = 0, greenMsMax = 0;
  uint32_t redMsMin   = 0, redMsMax   = 0;
  uint32_t yellowMsMin= 0, yellowMsMax= 0;

  uint16_t roundGoal = 100;
  uint32_t roundStartScore = 0;

  // --- Intermission between R2 and R3 (bonus-all for 10s) ---
  bool     bonusIntermission = false;
  uint32_t bonusInterStart   = 0;
  uint32_t bonusInterEnd     = 0;
  uint16_t bonusInterMs      = 15000;  // default 10s
  bool     bonusWarnTickStarted = false;

  // --- Intermission after R3 (R3.5): single-station bonus that hops every N ms ---
  bool     bonusIntermission2   = false;
  uint32_t bonus2Start          = 0;
  uint32_t bonus2End            = 0;
  uint16_t bonus2Ms             = 15000;   // 15 s window (like R2.5)
  uint16_t bonus2HopMs          = 3000;    // hop the bonus every 3 s
  uint8_t  bonus2Sid            = 0;       // currently highlighted station
  uint32_t bonus2NextHopAt      = 0;       // next hop time (millis)
  
  // R3.5 random hop order (shuffle without repeats)
  uint8_t  bonus2Order[MAX_STATIONS];  // 1..MAX_STATIONS
  uint8_t  bonus2Idx = 0;              // next index into bonus2Order

  // --- Bonus runtime state (cleared at round start) ---
  uint32_t bonusActiveMask = 0;                 // bit i => station i is bonus-active
  uint32_t bonusEndsAt[MAX_STATIONS] = {0};     // per-station TTL end time (millis)
  uint32_t bonusNextSpawnAt = 0;                // scheduler next fire (millis)
  uint8_t  bonusSpawnsThisRound = 0;            // number of spawns so far in current round

  // ---- Minigame (post-R4) ----
  struct MgConfig {
    uint32_t seed;
    uint16_t timerMs;
    uint8_t  speedMinMs, speedMaxMs;
    uint8_t  segMin, segMax;
  };

  bool     mgActive         = false;
  uint32_t mgStartedAt      = 0;
  uint32_t mgDeadline       = 0;     // mgStartedAt + timer
  uint32_t mgAllTriedAt     = 0;     // first time all stations have tried (for +3s end)
  uint32_t mgTriedMask      = 0;     // bit i => station i has used its attempt
  uint32_t mgSuccessMask    = 0;     // bit i => station i reported success
  uint8_t  mgExpectedStations = MAX_STATIONS; // set to 5 if you only have 5 Loots
  MgConfig mgCfg{};

  // ---- Round 5: Hot-station hop mechanic ----
  bool     r5Active          = false;
  uint8_t  r5HotSid          = 0;       // current “hot” station (1..5)
  uint8_t  r5Order[5]        = {1,2,3,4,5}; // current shuffle order
  uint8_t  r5Idx             = 0;       // index into r5Order
  uint32_t r5DwellEndAt      = 0;       // hop when now >= this
  uint32_t r5NextDepleteAt   = 0;       // throttle deplete steps

  // Tunables (defaults; change at runtime if you like)
  uint16_t r5DwellMinMs      = 4000;    // dwell window min (ms)
  uint16_t r5DwellMaxMs      = 9000;    // dwell window max (ms)
  uint16_t r5DepletePerStep  = 2;       // inventory units per deplete tick
  uint16_t r5DepleteStepMs   = 250;     // deplete every N ms when idle

  // Grace + PIR
  uint32_t edgeGraceMs     = 300;
  uint32_t redHoldGraceMs  = 400;
  uint32_t lastFlipMs      = 0;
  uint32_t redGraceUntil   = 0;

  uint8_t   roundIndex      = 1;
  uint32_t  gameStartAt     = 0;
  uint32_t  gameEndAt       = 0;
  uint32_t  roundStartAt    = 0;
  uint32_t  roundEndAt      = 0;
  bool      noRedThisRound  = true;     // Round 1 = true

  bool     pirEnforce      = true;
  uint32_t pirArmDelayMs   = 6000;
  uint32_t pirArmAt        = 0;

  // Drip broadcast
  PendingStart pending{};
  uint32_t lastTickSentMs = 0;

  // Tables
  PlayerRec players[MAX_PLAYERS];
  HoldRec   holds[MAX_HOLDS];
  PirRec    pir[4];
  uint16_t  stationCapacity[7]  = {0, 56,56,56,56,56, 0}; // index 0,6 unused
  uint16_t  stationInventory[7] = {0, 56,56,56,56,56, 0};
};

// Helpers
void resetGame(Game& g);
int  findPlayer(const Game& g, const TrexUid& u);
int  ensurePlayer(Game& g, const TrexUid& u);
int  findHoldById(const Game& g, uint32_t hid);
int  allocHold(Game& g);

// Lifecycle
void startNewGame(Game& g);
