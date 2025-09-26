#pragma once
#include <stdint.h>
#include <Adafruit_NeoPixel.h>
#include <TrexProtocol.h>   // LightState

// Provided by the sketch (defined in TREX_Loot.ino)
extern Adafruit_NeoPixel ring;   // 14 px ring
extern Adafruit_NeoPixel gauge;  // GAUGE_LEN px bar

// ---- LED state that other modules touch ----
extern bool     fullBlinkActive;
extern bool     fullBlinkOn;
extern uint32_t fullBlinkLastMs;
extern uint32_t blinkHoldId;

extern bool     yellowBlinkActive;
extern bool     yellowBlinkOn;
extern uint32_t yellowBlinkLastMs;

extern bool     emptyBlinkActive;
extern bool     emptyBlinkOn;
extern uint32_t emptyBlinkLastMs;

extern uint16_t   lastInvPainted;
extern uint16_t   lastCapPainted;
extern LightState lastGaugeColor;
extern bool       gaugeCacheValid;

extern uint32_t nextGaugeDrawAtMs;

// ---- LED API (names preserved) ----
uint32_t gaugeColor();

void fillRing(uint32_t c);
void fillGauge(uint32_t c);

void drawRingCarried(uint8_t cur, uint8_t maxC);
void drawGaugeInventory(uint16_t inventory, uint16_t capacity);
void drawGaugeInventoryRainbowAnimated(uint16_t inventory, uint16_t capacity, uint16_t phase);

// Auto-selects rainbow vs normal based on bonus & light state
void drawGaugeAuto(uint16_t inventory, uint16_t capacity);

// Bonus rainbow animation tick (call from loop)
void tickBonusRainbow();

// Full indicator (ring) blink
void startFullBlinkImmediate();
void stopFullBlink();
void tickFullBlink();

// Yellow gauge blink
void startYellowBlinkImmediate();
void stopYellowBlink();
void tickYellowBlink();

// Empty-station blink (LED 0 white)
void startEmptyBlink();
void stopEmptyBlink();
void tickEmptyBlink();

// Force a repaint now (honors all guards)
void forceGaugeRepaint();

// True unless OTA or game-over or Yellow OFF phase blocks painting
bool canPaintGaugeNow();

// Game-over visual (3 fast red blinks → off)
void gameOverBlinkAndOff();

// ---- OTA visual helpers (used by blocking URL OTA) ----
void otaVisualStart();
void otaTickSpinner();
void otaDrawProgress(uint32_t bytes, uint32_t total);
void otaVisualSuccess();
void otaVisualFail();

// ---- Minigame drawing ----
// Draws: black bar + static rainbow segment + a single cursor pixel in cursorColor.
// (No gating here—minigame owns the gauge while active.)
void mgDrawFrame(uint8_t segStart, uint8_t segLen, int16_t cursorIdx, uint32_t cursorColor);
