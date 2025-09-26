#include "LootLeds.h"
#include <Arduino.h>
#include <pgmspace.h>

// Fallbacks in case some macros are only in the sketch.
// These do NOT override your existing definitions.
#ifndef PIN_MOSFET
#define PIN_MOSFET 17
#endif
#ifndef GAUGE_LEN
#define GAUGE_LEN gauge.numPixels()
#endif
#ifndef OTA_SPINNER_MS
#define OTA_SPINNER_MS 100
#endif
#ifndef RING_STAGGER_MS
#define RING_STAGGER_MS 0
#endif
#ifndef EMPTY_STAGGER_MS
#define EMPTY_STAGGER_MS 0
#endif
#ifndef RING_BRIGHTNESS
#define RING_BRIGHTNESS 64
#endif

// ===== externs from other modules / sketch =====
extern bool      gameActive;
extern bool      otaInProgress;
extern bool      tagPresent;
extern bool      holdActive;
extern uint32_t  holdId;
extern uint8_t   carried, maxCarry;
extern uint16_t  inv, cap;
extern LightState g_lightState;

// Bonus state (lives with RX/audio; we only read)
extern bool      s_isBonusNow;
extern uint32_t  g_bonusExclusiveUntilMs;

// ===== color helpers (module-local) =====
static inline uint32_t C_RGB(uint8_t r,uint8_t g,uint8_t b){ return Adafruit_NeoPixel::Color(r,g,b); }
static const uint32_t RED   = C_RGB(255,  0,  0);
static const uint32_t GREEN = C_RGB(  0,255,  0);
static const uint32_t BLUE  = C_RGB(  0,  0,255);
static const uint32_t CYAN  = C_RGB(  0,200,255);
static const uint32_t YELLOW= C_RGB(255,180,  0);
static const uint32_t WHITE = C_RGB(255,255,255);
static const uint32_t OFF   = 0;

// ===== ring layout (pair-symmetric order) =====
static constexpr uint8_t RING_ROTATE = 0;
static const uint8_t ORDER_SYM_14[14] PROGMEM = { 0,1,13,2,12,3,11,4,10,5,9,6,8,7 };

// ===== timing constants (kept identical) =====
static constexpr uint16_t FULL_BLINK_PERIOD_MS   = 320;  // ~3 Hz
static constexpr uint16_t YELLOW_BLINK_PERIOD_MS = 500;
static constexpr uint16_t EMPTY_BLINK_PERIOD_MS  = 500;

static constexpr uint16_t RAINBOW_STEP     = 768; // ~fast smooth
static constexpr uint16_t RAINBOW_FRAME_MS = 33;  // ~30 FPS

// ===== LED state (definitions) =====
bool     fullBlinkActive = false;
bool     fullBlinkOn     = false;
uint32_t fullBlinkLastMs = 0;
uint32_t blinkHoldId     = 0;

bool     yellowBlinkActive = false;
bool     yellowBlinkOn     = false;
uint32_t yellowBlinkLastMs = 0;

bool     emptyBlinkActive = false;
bool     emptyBlinkOn     = false;
uint32_t emptyBlinkLastMs = 0;

static bool ringCarriedValid = false;

uint16_t   lastInvPainted = 0, lastCapPainted = 0;
LightState lastGaugeColor = LightState::GREEN;
bool       gaugeCacheValid = false;

uint32_t nextGaugeDrawAtMs = 0;

// Bonus animation phase (client-side)
static uint16_t g_rainbowPhase = 0;

// OTA spinner (visuals)
static bool     otaSpinnerActive = false;
static uint16_t otaSpinnerIdx    = 0;
static uint32_t otaSpinnerLastMs = 0;

// ===== LED drawing (moved verbatim) =====
uint32_t gaugeColor() {
  return (g_lightState == LightState::GREEN) ? GREEN : RED;
}

// Light the first nLit LEDs using the symmetric order, in strict pairs.
static void drawRingSymmetricLit(uint8_t nLit, uint32_t color) {
  if (nLit > 14) nLit = 14;
  // Enforce “two sides at once”: round down to even so pairs light together
  if (nLit & 1) nLit--;

  // Clear, then paint in our custom order
  for (uint8_t i = 0; i < 14; ++i) ring.setPixelColor(i, OFF);
  for (uint8_t i = 0; i < nLit; ++i) {
    uint8_t idx = pgm_read_byte(&ORDER_SYM_14[i]);
    idx = (idx + RING_ROTATE) % 14;
    ring.setPixelColor(idx, color);
  }
  ring.show();
}

void drawRingCarried(uint8_t cur, uint8_t maxC) {
  if (fullBlinkActive || otaInProgress) return;  // OTA owns the ring

  static uint8_t lastLit = 255;
  const uint16_t n = ring.numPixels(); // 14
  uint16_t lit = 0;
  if (maxC > 0) {
    // Same ceiling mapping you used before, just reusing the math
    lit = (uint16_t)((uint32_t)cur * n + (maxC - 1)) / maxC;
  }

  // Force pairwise advance so both arcs fill at the same time
  if (lit & 1) lit--;
  if (ringCarriedValid && lit == lastLit) return;
  lastLit = lit;

  drawRingSymmetricLit((uint8_t)lit, GREEN);
  ringCarriedValid = true;
}

void drawGaugeInventory(uint16_t inventory, uint16_t capacity) {
  if (otaInProgress) return;
  const LightState colorState = g_lightState;
  const bool offPhase = (colorState == LightState::YELLOW && yellowBlinkActive && !yellowBlinkOn);

  // Locals cached across calls to avoid redundant work
  static bool     lastOffPhase   = false;
  static uint16_t lastLitPainted = 0xFFFF;  // impossible value forces first paint
  static bool     lastLampOn     = false;

  // ----- EXTREME GUARD: empty -----
  if (inventory == 0) {
    // Only repaint if something relevant changed (phase/color/capacity or we weren't already empty)
    if (!(gaugeCacheValid && lastInvPainted == 0 &&
          lastCapPainted == capacity && lastGaugeColor == colorState &&
          lastOffPhase == offPhase)) {

      if (emptyBlinkActive && tagPresent && !offPhase) {
        // Show the “empty” overlay pixel
        fillGauge(OFF);
        gauge.setPixelColor(0, emptyBlinkOn ? WHITE : OFF);
        gauge.show();
      } else {
        fillGauge(OFF);
        gauge.show();
      }

      lastInvPainted   = 0;
      lastCapPainted   = capacity;
      lastGaugeColor   = colorState;
      lastLitPainted   = 0;
      lastOffPhase     = offPhase;
      gaugeCacheValid  = true;
    }

    // Lamp OFF only if we weren’t already off
    if (lastLampOn) { digitalWrite(PIN_MOSFET, LOW); lastLampOn = false; }
    return;
  }

  // ----- YELLOW off-phase: keep bar dark but only paint on phase edges -----
  if (offPhase) {
    if (!gaugeCacheValid || !lastOffPhase) {
      fillGauge(OFF);
      gauge.show();
      gaugeCacheValid = true;
      lastOffPhase    = true;
      // keep other caches consistent
      lastInvPainted  = inventory;
      lastCapPainted  = capacity;
      lastGaugeColor  = colorState;
      lastLitPainted  = 0;
    }
    // Lamp ON when not empty (avoid redundant write)
    if (!lastLampOn) { digitalWrite(PIN_MOSFET, HIGH); lastLampOn = true; }
    return;
  } else if (lastOffPhase) {
    // We’re leaving off-phase; force one repaint
    lastOffPhase    = false;
    gaugeCacheValid = false;
  }

  // Clamp lit count (1:1 mapping; never > GAUGE_LEN)
  const uint16_t lit = (inventory > GAUGE_LEN) ? GAUGE_LEN : inventory;

  // Choose color by current light
  uint32_t col = RED;
  if      (colorState == LightState::GREEN)  col = GREEN;
  else if (colorState == LightState::YELLOW) col = YELLOW;

  // Skip if nothing visible changed (lit length, capacity, or color)
  if (gaugeCacheValid &&
      lit == lastLitPainted &&
      capacity == lastCapPainted &&
      colorState == lastGaugeColor) {
    // Ensure lamp ON (since inventory > 0)
    if (!lastLampOn) { digitalWrite(PIN_MOSFET, HIGH); lastLampOn = true; }
    return;
  }

  // Draw (bounded, no edge writes)
  for (uint16_t i = 0; i < GAUGE_LEN; ++i) {
    gauge.setPixelColor(i, (i < lit) ? col : OFF);
  }
  gauge.show();

  // Lamp follows inventory (ON when not empty); avoid redundant write
  if (!lastLampOn) { digitalWrite(PIN_MOSFET, HIGH); lastLampOn = true; }

  lastInvPainted   = inventory;
  lastCapPainted   = capacity;
  lastGaugeColor   = colorState;
  lastLitPainted   = lit;
  gaugeCacheValid  = true;
}

void drawGaugeInventoryRainbowAnimated(uint16_t inventory, uint16_t capacity, uint16_t phase) {
  if (otaInProgress) return;
  if (g_lightState != LightState::GREEN) { drawGaugeInventory(inventory, capacity); return; }

  uint16_t lit = (inventory > GAUGE_LEN) ? GAUGE_LEN : inventory;

  // Dense rainbow: double the spatial frequency for extra pop
  for (uint16_t i = 0; i < GAUGE_LEN; ++i) {
    if (i < lit) {
      uint32_t baseHue = ((uint32_t)i * 2u * 65535u) / GAUGE_LEN; // ×2 density
      uint16_t hue     = (uint16_t)(baseHue + phase);
      uint32_t c = gauge.ColorHSV(hue, /*sat*/255, /*val*/255);
      gauge.setPixelColor(i, c);
    } else {
      gauge.setPixelColor(i, 0);
    }
  }

  // Keep empty overlay behavior
  if (emptyBlinkActive && tagPresent && inventory == 0) {
    gauge.setPixelColor(0, emptyBlinkOn ? WHITE : OFF);
  }

  gauge.show();

  if (inventory == 0) digitalWrite(PIN_MOSFET, LOW);
  else                digitalWrite(PIN_MOSFET, HIGH);
}

// Only show rainbow when BONUS is active, there is inventory, and we are GREEN.
// Otherwise fall back to the normal draw (YELLOW and RED always override).
void drawGaugeAuto(uint16_t inventory, uint16_t capacity) {
  if (s_isBonusNow && inventory > 0 && g_lightState == LightState::GREEN) {
    drawGaugeInventoryRainbowAnimated(inventory, capacity, g_rainbowPhase);
  } else {
    drawGaugeInventory(inventory, capacity);
  }
}

void tickBonusRainbow() {
  // Only animate when: game active, bonus on THIS station, we have inventory, and light is GREEN
  if (!(gameActive && s_isBonusNow && inv > 0 && g_lightState == LightState::GREEN)) return;

  uint32_t now = millis();
  if ((int32_t)(now - nextGaugeDrawAtMs) < 0) return;  // reuse your 20ms throttle

  g_rainbowPhase += RAINBOW_STEP;                      // scroll the hues
  drawGaugeInventoryRainbowAnimated(inv, cap, g_rainbowPhase);

  // Pick frame spacing: gentle during the exclusive window, then normal
  const uint16_t frameMs = (millis() < g_bonusExclusiveUntilMs) ? 60 : RAINBOW_FRAME_MS;
  nextGaugeDrawAtMs = now + frameMs;
}

void fillRing(uint32_t c) {
  ringCarriedValid = false;
  for (uint16_t i=0;i<ring.numPixels();++i) ring.setPixelColor(i,c);
  ring.show();
}

void fillGauge(uint32_t c) {
  for (uint16_t i=0;i<GAUGE_LEN;++i) gauge.setPixelColor(i,c);

  // Inject overlay into this same frame (no second show)
  if (emptyBlinkActive && tagPresent && inv == 0) {
    if (!(g_lightState == LightState::YELLOW && yellowBlinkActive && !yellowBlinkOn)) {
      gauge.setPixelColor(0, emptyBlinkOn ? WHITE : OFF);
    }
  }

  gauge.show();
  gaugeCacheValid = false;
}

// Gate gauge paints to avoid OTA/RED state thrash and YELLOW off-phase frames.
bool canPaintGaugeNow() {
  if (otaInProgress)        return false;            // OTA visuals own the LEDs
  if (!gameActive)          return false;            // post-GAME_OVER stays dark
  // Skip during YELLOW blink "off" phase; drawGaugeInventory() will handle edges.
  if (g_lightState == LightState::YELLOW && yellowBlinkActive && !yellowBlinkOn)
    return false;
  return true;
}

// Force a repaint immediately (used by empty/yellow blink toggles, etc.).
void forceGaugeRepaint() {
  if (otaInProgress) return;                         // don’t fight OTA spinner
  gaugeCacheValid = false;                           // invalidate cache so we repaint
  drawGaugeAuto(inv, cap);                           // respects current light/bonus
}

// ===== Full / Yellow / Empty blinks =====
void startFullBlinkImmediate() {
  fullBlinkActive = true;
  fullBlinkOn     = true;
  fullBlinkLastMs = millis();
  blinkHoldId     = holdId;
  fillRing(YELLOW);
}
void stopFullBlink() { fullBlinkActive = false; fullBlinkOn = false; }
void tickFullBlink() {
  if (!fullBlinkActive) return;
  uint32_t now = millis();
  if ((now - fullBlinkLastMs) >= FULL_BLINK_PERIOD_MS) {
    fullBlinkLastMs = now;
    fullBlinkOn = !fullBlinkOn;
    fillRing(fullBlinkOn ? YELLOW : OFF);
  } else {
    // drip-feed between flips if needed
  }
}

void startYellowBlinkImmediate() {
  yellowBlinkActive = true;
  yellowBlinkOn     = true;
  yellowBlinkLastMs = millis() + RING_STAGGER_MS;  // ← stagger start
  // Paint immediately in current color (YELLOW) at current inv level
  drawGaugeInventory(inv, cap);
}
void stopYellowBlink() { yellowBlinkActive = false; yellowBlinkOn = false; }
void tickYellowBlink() {
  // don’t blink the gauge while in a regular BONUS and light is GREEN
  if (s_isBonusNow && g_lightState == LightState::GREEN) return;

  if (!yellowBlinkActive) return;
  uint32_t now = millis();
  if ((now - yellowBlinkLastMs) >= YELLOW_BLINK_PERIOD_MS) {
    yellowBlinkLastMs = now;
    yellowBlinkOn = !yellowBlinkOn;
    if (yellowBlinkOn) {
      drawGaugeInventory(inv, cap);
    } else {
      fillGauge(OFF);
    }
  }
}

static void applyEmptyOverlay() {
  // Overlay only if actively scanning an empty station (and not OTA)
  if (!emptyBlinkActive || otaInProgress || !tagPresent || inv != 0) return;
  // Respect YELLOW off-phase:
  if (g_lightState == LightState::YELLOW && yellowBlinkActive && !yellowBlinkOn) return;
  // Paint just LED 0: white when ON, off when OFF
  gauge.setPixelColor(0, emptyBlinkOn ? WHITE : OFF);
  gauge.show();
}

void startEmptyBlink() {
  emptyBlinkActive = true;
  emptyBlinkOn     = true;
  emptyBlinkLastMs = millis() + EMPTY_STAGGER_MS;  // ← stagger start
  forceGaugeRepaint();                             // immediate visual
}
void stopEmptyBlink() {
  if (!emptyBlinkActive) return;
  emptyBlinkActive = false;
  emptyBlinkOn     = false;
  forceGaugeRepaint();
}
void tickEmptyBlink() {
  if (!emptyBlinkActive) return;
  uint32_t now = millis();
  if ((now - emptyBlinkLastMs) >= EMPTY_BLINK_PERIOD_MS) {
    emptyBlinkLastMs = now;
    emptyBlinkOn = !emptyBlinkOn;
    forceGaugeRepaint();
  } else {
    // drip-feed if you later add effects
  }
}

// ===== Game-over visual =====
void gameOverBlinkAndOff() {
  // 3 quick red blinks on ring + gauge + MOSFET, then off
  const int cycles = 3;
  for (int k = 0;k < cycles; ++k) {
    // ON
    fillGauge(RED);
    digitalWrite(PIN_MOSFET, HIGH);
    uint32_t t = millis();
    while (millis() - t < 500);

    // OFF
    fillGauge(OFF);
    digitalWrite(PIN_MOSFET, LOW);
    t = millis();
    while (millis() - t < 500);
  }
  // Final state: fully off
  fillGauge(OFF);
  digitalWrite(PIN_MOSFET, LOW);
}

// ===== OTA visuals (spinner + progress + success/fail) =====
void otaVisualStart() {
  otaSpinnerActive = true;
  otaSpinnerIdx = 0;
  otaSpinnerLastMs = millis();
  fillGauge(OFF);
  // quick cyan breathe (~1s)
  for (int b=0; b<=255; b+=25) { ring.setBrightness(b); fillRing(CYAN); delay(20); }
  for (int b=255; b>=RING_BRIGHTNESS; b-=25){ ring.setBrightness(b); fillRing(CYAN); delay(20); }
  ring.setBrightness(RING_BRIGHTNESS);
}

void otaTickSpinner() {
  if (!otaSpinnerActive) return;
  uint32_t now = millis();
  if (now - otaSpinnerLastMs < OTA_SPINNER_MS) return;
  otaSpinnerLastMs = now;
  // one blue pixel walks the ring
  for (uint16_t i=0;i<ring.numPixels();++i)
    ring.setPixelColor(i, (i==otaSpinnerIdx) ? BLUE : OFF);
  ring.show();
  otaSpinnerIdx = (otaSpinnerIdx + 1) % ring.numPixels();
}

void otaDrawProgress(uint32_t bytes, uint32_t total) {
  if (total == 0) return; // unknown
  uint16_t lit = (uint16_t)((uint64_t)bytes * GAUGE_LEN / total);
  for (uint16_t i=0;i<GAUGE_LEN;++i)
    gauge.setPixelColor(i, (i<lit) ? BLUE : OFF);
  gauge.show();
}

void otaVisualSuccess() {
  otaSpinnerActive = false;
  // green flash
  fillRing(GREEN);
  // yellow sweep
  for (uint16_t i=0;i<GAUGE_LEN;++i) { gauge.setPixelColor(i, YELLOW); gauge.show(); delay(3); }
}

void otaVisualFail() {
  otaSpinnerActive = false;
  for (int i=0;i<6;i++) { fillRing(RED); delay(120); fillRing(OFF); delay(80); }
  fillGauge(OFF);
}
