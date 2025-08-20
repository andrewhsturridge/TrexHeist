#include "Cadence.h"
#include "Media.h"

void enterGreen(Game& g) {
  g.light = LightState::GREEN;
  g.nextSwitch = millis() + g.greenMs;
  g.lastFlipMs = millis();
  spritePlay(CLIP_NOT_LOOKING);
  Serial.println("[TREX] -> GREEN");
}

void enterRed(Game& g) {
  g.light  = LightState::RED;
  g.nextSwitch  = millis() + g.redMs;
  g.lastFlipMs  = millis();
  g.redGraceUntil = g.lastFlipMs + g.redHoldGraceMs;
  g.pirArmAt      = g.lastFlipMs + g.pirArmDelayMs;
  spritePlay(CLIP_LOOKING);
  Serial.println("[TREX] -> RED");
  // NOTE: we do NOT end game here; the main loop checks after grace
}

void tickCadence(Game& g, uint32_t now) {
  // Warmup suppresses flips; end warmup then apply first level values
  if (g.warmupActive) {
    if (now >= g.warmupEndAt) {
      g.warmupActive = false;
      // Keep current light; next flip uses current timers already set
      Serial.println("[TREX] Warmup ended → classic Level 1");
    }
    return;
  }

  if (g.phase != Phase::PLAYING) return;
  if (now < g.nextSwitch) return;

  (g.light == LightState::GREEN) ? enterRed(g) : enterGreen(g);
}
