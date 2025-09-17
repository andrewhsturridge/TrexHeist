#include <esp_random.h>
#include "Cadence.h"
#include "Media.h"
#include "Net.h"
#include "GameAudio.h"
#include "Bonus.h"

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
  if (gameAudioCurrentTrack() != TRK_TREX_WIN) {
    gameAudioStop();
  }
  // Immediate state broadcast
  uint32_t now = millis();
  uint32_t msLeft = (g.nextSwitch > now) ? (g.nextSwitch - now) : 0;
  uint32_t toRoundEnd = (g.roundEndAt > now) ? (g.roundEndAt - now) : 0xFFFFFFFFUL;
  uint32_t toGameEnd  = (g.gameEndAt  > now) ? (g.gameEndAt  - now) : 0xFFFFFFFFUL;
  if (toRoundEnd < msLeft) msLeft = toRoundEnd;
  if (toGameEnd  < msLeft) msLeft = toGameEnd;
  sendStateTick(g, msLeft);
}

void enterYellow(Game& g) {
  g.light      = LightState::YELLOW;
  uint32_t now = millis();
  g.lastFlipMs = now;

  if (g.roundIndex == 4) {
    const bool bounce = ((esp_random() % 100) < 50); // ~25% fake-out
    const uint32_t yBase = g.yellowMs ? g.yellowMs : 3000; // RED path = exactly this
    if (bounce) {
      // Use your 1500–3000 window but clamp max to (yBase - 1) to avoid overlap
      uint32_t yMin = g.yellowMsMin ? g.yellowMsMin : 1500;
      uint32_t yMax = g.yellowMsMax ? g.yellowMsMax : 3000;
      if (yMax >= yBase) yMax = (yBase > 0 ? yBase - 1 : 0); // ⇒ 1500..2999
      if (yMin > yMax)   yMin = yMax;                        // safety clamp
      g.nextSwitch = now + pickDur(/*base*/0, yMin, yMax);
    } else {
      // Non-bounce path: fixed yBase (e.g., 3000 ms) ⇒ will go to RED
      g.nextSwitch = now + yBase;
    }
  } else {
    g.nextSwitch = now + pickDur(g.yellowMs, g.yellowMsMin, g.yellowMsMax);
  }

  Serial.println("[TREX] -> YELLOW");
  gameAudioPlayOnce(TRK_TICKS_LOOP);

  uint32_t msLeft = (g.nextSwitch > now) ? (g.nextSwitch - now) : 0;
  uint32_t toRoundEnd = (g.roundEndAt > now) ? (g.roundEndAt - now) : 0xFFFFFFFFUL;
  uint32_t toGameEnd  = (g.gameEndAt  > now) ? (g.gameEndAt  - now) : 0xFFFFFFFFUL;
  if (toRoundEnd < msLeft) msLeft = toRoundEnd;
  if (toGameEnd  < msLeft) msLeft = toGameEnd;
  sendStateTick(g, msLeft);
}

void enterRed(Game& g) {
  g.light  = LightState::RED;
  g.nextSwitch  = millis() + pickDur(g.redMs, g.redMsMin, g.redMsMax);
  g.lastFlipMs  = millis();
  g.redGraceUntil = g.lastFlipMs + g.redHoldGraceMs;
  g.pirArmAt      = g.lastFlipMs + g.pirArmDelayMs;
  spritePlay(CLIP_LOOKING);
  Serial.println("[TREX] -> RED");
  gameAudioPlayOnce(TRK_PLAYERS_STAY_STILL);
  uint32_t now = millis();
  uint32_t msLeft = (g.nextSwitch > now) ? (g.nextSwitch - now) : 0;
  uint32_t toRoundEnd = (g.roundEndAt > now) ? (g.roundEndAt - now) : 0xFFFFFFFFUL;
  uint32_t toGameEnd  = (g.gameEndAt  > now) ? (g.gameEndAt  - now) : 0xFFFFFFFFUL;
  if (toRoundEnd < msLeft) msLeft = toRoundEnd;
  if (toGameEnd  < msLeft) msLeft = toGameEnd;
  sendStateTick(g, msLeft);
}

void tickCadence(Game& g, uint32_t now) {
  if (g.phase != Phase::PLAYING) return;

  // bonus scheduler/expiry
  tickBonusDirector(g, now);

  if (now < g.nextSwitch) return;

  if (g.noRedThisRound) {
    if (g.allowYellowThisRound) {
      (g.light == LightState::GREEN) ? enterYellow(g) : enterGreen(g);
    } else {
      enterGreen(g);
    }
    return;
  }

  if (g.light == LightState::GREEN) { enterYellow(g); return; }

  if (g.light == LightState::YELLOW) {
    if (g.roundIndex == 4) {
      const uint32_t yBase = g.yellowMs ? g.yellowMs : 3000;
      const uint32_t dur   = g.nextSwitch - g.lastFlipMs;
      (dur < yBase) ? enterGreen(g) : enterRed(g);
    } else {
      enterRed(g);
    }
    return;
  }

  enterGreen(g);
}
