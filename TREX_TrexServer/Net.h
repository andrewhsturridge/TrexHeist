#pragma once
#include "GameModel.h"
#include "ServerConfig.h"

// Broadcasts
void sendStateTick(const Game& g, uint32_t msLeft);
void bcastGameStart(Game& g);
void bcastGameOver(Game& g, uint8_t reason, uint8_t blameSid = GAMEOVER_BLAME_ALL);
void bcastScore(Game& g);
void bcastStation(Game& g, uint8_t stationId);
// Broadcast that a drop has completed
void sendDropResult(Game& g, uint16_t dropped, uint8_t readerIndex = DROP_READER_UNKNOWN);

void bcastRoundStatus(Game& g);
void bcastBonusUpdate(Game& g);
void bcastGameStatus(const Game& g);

// Lives system
enum class LifeLossResult : uint8_t { IGNORED=0, LIFE_LOST=1, GAME_OVER=2 };
void bcastLivesUpdate(Game& g, uint8_t reason = 0, uint8_t blameSid = GAMEOVER_BLAME_ALL);
LifeLossResult applyLifeLoss(Game& g, uint8_t reason, uint8_t blameSid = GAMEOVER_BLAME_ALL, bool obeyLockout = true);

// Minigame broadcasts
void bcastMgStart(Game& g, const Game::MgConfig& cfg);
void bcastMgStop(Game& g);

// Point messages
void sendHoldEnd(Game& g, uint32_t holdId, uint8_t reason);
void sendLootTick(Game& g, uint32_t holdId, uint8_t carried, uint16_t stationInv);

// RX dispatcher
void onRx(const uint8_t* data, uint16_t len);

// Maintenance / control helpers
bool netConsumeEnterMaintRequest();
bool netConsumeControlStartRequest();
bool netConsumeControlStopRequest();
bool netConsumeLootOtaRequest();

// Generic raw broadcast used by OTA
void netBroadcastRaw(const uint8_t* data, uint16_t len);
