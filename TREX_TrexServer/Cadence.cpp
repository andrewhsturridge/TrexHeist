#include <esp_random.h>
#include "Cadence.h"
#include "Media.h"
#include "Net.h"
#include "GameAudio.h"

static inline uint32_t pickDur(uint32_t base, uint32_t mn, uint32_t mx) {
  if (mn && mx && mx >= mn) {
    uint32_t span = mx - mn + 1;
    return mn + (esp_random() % span);
  }
  return base;
}

void enterGreen(Game& g) {
  g.light = LightState::GREEN;
  g.nextSwitch = millis() + pickDur(g.greenMs, g.greenMsMin, g.greenMsMax);
  g.lastFlipMs = millis();
  spritePlay(CLIP_NOT_LOOKING);
  Serial.println("[TREX] -> GREEN");
  // Immediate state broadcast
  uint32_t now = millis();
  uint32_t msLeft = (g.nextSwitch > now) ? (g.nextSwitch - now) : 0;
  uint32_t toRoundEnd = (g.roundEndAt > now) ? (g.roundEndAt - now) : 0xFFFFFFFFUL;
  uint32_t toGameEnd  = (g.gameEndAt  > now) ? (g.gameEndAt  - now) : 0xFFFFFFFFUL;
  if (toRoundEnd < msLeft) msLeft = toRoundEnd;
  if (toGameEnd  < msLeft) msLeft = toGameEnd;
  sendStateTick(g, msLeft);
  gameAudioStopIfLooping();
}


void enterYellow(Game& g) {
  g.light = LightState::YELLOW;
  g.nextSwitch = millis() + pickDur(g.yellowMs, g.yellowMsMin, g.yellowMsMax);
  g.lastFlipMs = millis();
  Serial.println("[TREX] -> YELLOW");
  uint32_t now = millis();
  uint32_t msLeft = (g.nextSwitch > now) ? (g.nextSwitch - now) : 0;
  uint32_t toRoundEnd = (g.roundEndAt > now) ? (g.roundEndAt - now) : 0xFFFFFFFFUL;
  uint32_t toGameEnd  = (g.gameEndAt  > now) ? (g.gameEndAt  - now) : 0xFFFFFFFFUL;
  if (toRoundEnd < msLeft) msLeft = toRoundEnd;
  if (toGameEnd  < msLeft) msLeft = toGameEnd;
  sendStateTick(g, msLeft);
  gameAudioPlayLoop(TRK_TICKS_LOOP);
}

void enterRed(Game& g) {
  g.light  = LightState::RED;
  g.nextSwitch  = millis() + pickDur(g.redMs, g.redMsMin, g.redMsMax);
  g.lastFlipMs  = millis();
  g.redGraceUntil = g.lastFlipMs + g.redHoldGraceMs;
  g.pirArmAt      = g.lastFlipMs + g.pirArmDelayMs;
  spritePlay(CLIP_LOOKING);
  Serial.println("[TREX] -> RED");
  uint32_t now = millis();
  uint32_t msLeft = (g.nextSwitch > now) ? (g.nextSwitch - now) : 0;
  uint32_t toRoundEnd = (g.roundEndAt > now) ? (g.roundEndAt - now) : 0xFFFFFFFFUL;
  uint32_t toGameEnd  = (g.gameEndAt  > now) ? (g.gameEndAt  - now) : 0xFFFFFFFFUL;
  if (toRoundEnd < msLeft) msLeft = toRoundEnd;
  if (toGameEnd  < msLeft) msLeft = toGameEnd;
  sendStateTick(g, msLeft);
  gameAudioPlayOnce(TRK_PLAYERS_STAY_STILL);
}

void tickCadence(Game& g, uint32_t now) {
  if (g.phase != Phase::PLAYING) return;

  gameAudioLoopTick();

  // Round 1: GREEN only (no YELLOW, no RED)
  if (g.noRedThisRound && !g.allowYellowThisRound) {
    if (g.light != LightState::GREEN) {
      enterGreen(g);                  // force GREEN once if not already
    }
    g.nextSwitch = now + 3600000UL;   // push next flip far out so we don't re-enter
    return;                           // nothing else to do this tick
  }

  if (now < g.nextSwitch) return;

  if (g.noRedThisRound) {
    // Round 1 previously GREEN<->YELLOW; now won't run because of guard above.
    // If you ever use "noRedThisRound=true AND allowYellowThisRound=true",
    // this branch will toggle GREEN <-> YELLOW:
    (g.light == LightState::GREEN) ? enterYellow(g) : enterGreen(g);
  } else {
    // Full cadence: GREEN → YELLOW → RED → GREEN
    if      (g.light == LightState::GREEN)  enterYellow(g);
    else if (g.light == LightState::YELLOW) enterRed(g);
    else                                    enterGreen(g);
  }
}
