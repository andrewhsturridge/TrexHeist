#pragma once
#include "GameModel.h"
#include "ServerConfig.h"

// Broadcasts
void sendStateTick(const Game& g, uint32_t msLeft);
void bcastGameStart(Game& g);
void bcastGameOver(Game& g, uint8_t reason);
void bcastScore(Game& g);
void bcastStation(Game& g, uint8_t stationId);
void sendDropResult(Game& g, uint16_t dropped);

// Point messages
void sendHoldEnd(Game& g, uint32_t holdId, uint8_t reason);
void sendLootTick(Game& g, uint32_t holdId, uint8_t carried, uint16_t stationInv);

// RX dispatcher
void onRx(const uint8_t* data, uint16_t len);
