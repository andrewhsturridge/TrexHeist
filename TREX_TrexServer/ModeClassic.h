#pragma once
#include "GameModel.h"

// Configure warmup + level table (fixed intervals that speed up)
void modeClassicInit(Game& g);
void modeClassicMaybeAdvance(Game& g);
// Add near other declarations
void modeClassicForceRound(Game& g, uint8_t idx, bool playWin = true);
void modeClassicNextRound(Game& g, bool playWin = true);
// R2.5 bonus intermission (fills all, auto-drains to 0, then enters R3)
void startBonusIntermission(Game& g, uint16_t durationMs = 15000);
void tickBonusIntermission(Game& g, uint32_t now);
// R3.5 bonus intermission: 1 station active; hops every hopMs; ends → Round 4
void startBonusIntermission2(Game& g, uint16_t durationMs = 15000, uint16_t hopMs = 3000);
void tickBonusIntermission2(Game& g, uint32_t now);
