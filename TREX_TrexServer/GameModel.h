#pragma once
#include <Arduino.h>
#include <TrexProtocol.h>

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
  uint32_t greenMs = 10000;
  uint32_t redMs   = 8000;
  uint32_t yellowMs = 1500;
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
