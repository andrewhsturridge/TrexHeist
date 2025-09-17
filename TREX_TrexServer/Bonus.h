#pragma once
#include "GameModel.h"

// Call when a new round starts (R3/R4 allowed)
void bonusResetForRound(Game& g, uint32_t now);

// Manual triggers (for serial)
void bonusForceSpawn(Game& g, uint32_t now);     // spawn per current round rules
void bonusClearAll(Game& g);                     // clear mask immediately

// Call every server tick to expire/spawn bonuses (idempotent/cheap)
void tickBonusDirector(Game& g, uint32_t now);

// Apply "instant-drain" at LOOT_HOLD_START if station is bonus.
// Returns true if the hold ended immediately (FULL or EMPTY).
bool applyBonusOnHoldStart(Game& g, uint8_t playerIdx, uint8_t stationId, uint32_t holdId);
