#pragma once
#include "GameModel.h"

// Cadence flips + grace and PIR arming
void enterGreen(Game& g);
void enterRed(Game& g);
void tickCadence(Game& g, uint32_t now);
