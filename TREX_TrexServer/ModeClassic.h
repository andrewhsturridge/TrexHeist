#pragma once
#include "GameModel.h"

// Configure warmup + level table (fixed intervals that speed up)
void modeClassicInit(Game& g);
void modeClassicMaybeAdvance(Game& g);
// Add near other declarations
void modeClassicForceRound(Game& g, uint8_t idx, bool playWin = true);
void modeClassicNextRound(Game& g, bool playWin = true);

