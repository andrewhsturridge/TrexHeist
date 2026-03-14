#pragma once
#include "ServerConfig.h"

constexpr uint8_t CLIP_NOT_LOOKING = 0;  // GREEN loop
constexpr uint8_t CLIP_LOOKING     = 1;  // RED loop
constexpr uint8_t CLIP_GAME_OVER   = 2;  // fail / manual stop one-shot
constexpr uint8_t CLIP_LUNCHBREAK  = 3;  // lunchbreak bonus
constexpr uint8_t CLIP_SUCCESS     = 4;  // success one-shot (adjust if your Sprite asset index differs)

void mediaInit();
void spritePlay(uint8_t clip);
