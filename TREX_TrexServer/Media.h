#pragma once
#include "ServerConfig.h"

constexpr uint8_t CLIP_NOT_LOOKING = 0;  // GREEN loop
constexpr uint8_t CLIP_LOOKING     = 1;  // RED loop
constexpr uint8_t CLIP_GAME_OVER   = 2;  // one-shot
constexpr uint8_t CLIP_LUNCHBREAK  = 3;  // lunchbreak bonus

void mediaInit();
void spritePlay(uint8_t clip);
