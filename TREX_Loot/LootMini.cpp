#include "LootMini.h"
#include <Arduino.h>
#include <MFRC522.h>
#include "LootLeds.h"   // mg/viz helpers
#include "LootNet.h"    // sendMgResult(...)
#include "Audio.h"      // startLootAudio(true), stopAudio()
#include "Identity.h"   // STATION_ID
#include <TrexProtocol.h>

// --- externs provided elsewhere (unchanged) ---
extern MFRC522 rfid;
extern bool isAnyCardPresent(MFRC522 &m);
extern bool readUid(MFRC522 &m, TrexUid &out);

// Game/light state (read-only here)
extern volatile bool gameActive;
extern bool          otaInProgress;

// Public flag lives in the .ino; we just extern it here.
extern volatile bool mgActive;

// ----- local MG state -----
namespace {
  enum class MgState : uint8_t { Idle, Running, Success, Miss, Frozen }; // Frozen = timer over, frame held
  MgState   st = MgState::Idle;

  // Segment + cursor
  uint8_t   segStart = 0;
  uint8_t   segLen   = 0;      // >=1
  int16_t   cursor   = 0;      // 0..N-1
  int8_t    dir      = +1;     // bounce
  uint16_t  stepMs   = 40;     // cursor step interval
  uint32_t  nextStepAt = 0;

  // Duration
  uint32_t  endAtMs  = 0;

  // Try handling
  bool      tried    = false;
  bool      tagPrev  = false;
  TrexUid   triedUid{};

  // Miss blink
  bool      missOn = false;
  uint16_t  missPeriodMs = 220;
  uint32_t  nextBlinkAt = 0;

  // Tiny LCG RNG so we don't disturb global random()
  uint32_t  rng = 1;
  static inline void rngSeed(uint32_t s) { rng = s ? s : 1; }
  static inline uint32_t rngNext() { rng = (1103515245u * rng + 12345u); return rng; }
  static inline uint32_t rngRange(uint32_t lo, uint32_t hiIncl) {
    if (hiIncl <= lo) return lo;
    uint32_t span = (hiIncl - lo + 1);
    return lo + (rngNext() % span);
  }

  static uint16_t gaugeLen() { return (uint16_t)gauge.numPixels(); }

  static inline bool inSeg(uint16_t idx) {
    return (idx >= segStart) && (idx < (uint16_t)(segStart + segLen));
  }

  static void drawRunning(uint32_t now) {
    (void)now;
    // Draw current frame: rainbow segment + green cursor
    mgDrawFrame(segStart, segLen, cursor, /*cursorColor*/ Adafruit_NeoPixel::Color(0,255,0)); // GREEN
  }

  static void drawSuccess() {
    // Solid green at the cursor; keep segment visible
    mgDrawFrame(segStart, segLen, cursor, Adafruit_NeoPixel::Color(0,255,0));
  }

  static void drawMiss(uint32_t now) {
    // Blink red ON/OFF at cursor; keep segment visible
    if ((int32_t)(now - nextBlinkAt) >= 0) {
      nextBlinkAt = now + missPeriodMs;
      missOn = !missOn;
    }
    mgDrawFrame(segStart, segLen, cursor, missOn ? Adafruit_NeoPixel::Color(255,0,0) : 0); // RED / OFF
  }

  static void freezeFrame() { mgDrawFrame(segStart, segLen, cursor, Adafruit_NeoPixel::Color(0,255,0)); }

} // namespace

void mgStart(const MgParams& p) {
  if (otaInProgress) return;   // don't start if OTA owns LEDs

  // Seed per station for reproducibility
  rngSeed(p.seed ^ ((uint32_t)STATION_ID * 0x9E3779B1u));

  const uint16_t N = gaugeLen();
  uint8_t sMin = p.segMin ? p.segMin : 6;
  uint8_t sMax = p.segMax ? p.segMax : 16;
  if (sMin > sMax) sMin = sMax;
  if (sMax > N)    sMax = (uint8_t)N;
  segLen   = (uint8_t)rngRange(sMin, sMax);
  if (segLen == 0) segLen = 1;
  if (segLen > N)  segLen = (uint8_t)N;
  segStart = (uint8_t)rngRange(0, (uint32_t)(N - segLen));

  // Cursor speed
  uint8_t vMin = p.speedMinMs ? p.speedMinMs : 20;
  uint8_t vMax = p.speedMaxMs ? p.speedMaxMs : 80;
  if (vMin > vMax) vMin = vMax;
  stepMs   = (uint16_t)rngRange(vMin, vMax);

  // Cursor start + direction
  cursor = (int16_t)rngRange(0, (uint32_t)(N - 1));
  dir    = (rngNext() & 1) ? +1 : -1;

  // Timing
  endAtMs = millis() + (p.timerMs ? p.timerMs : 60000u);
  nextStepAt = millis() + stepMs;
  missOn = false;
  nextBlinkAt = millis() + missPeriodMs;

  tried   = false;
  tagPrev = false;
  st      = MgState::Running;
  mgActive = true;

  // Ensure audio doesn't interfere at start
  stopAudio();

  // Initial paint
  mgDrawFrame(segStart, segLen, cursor, Adafruit_NeoPixel::Color(0,255,0)); // GREEN
}

void mgStop() {
  // Release ownership and clear visuals
  st = MgState::Idle;
  mgActive = false;
  // Leave whatever frame was last shown; normal paints will resume.
}

void mgCancel() {
  st = MgState::Idle;
  mgActive = false;
}

void mgLoop() {
  if (!mgActive || otaInProgress) return;

  uint32_t now = millis();

  // Timer expiry: freeze frame, but keep mgActive so normal repaint is still gated
  if (st == MgState::Running && (int32_t)(now - endAtMs) >= 0) {
    st = MgState::Frozen;
    freezeFrame();
  }

  // Animate if still running
  if (st == MgState::Running && (int32_t)(now - nextStepAt) >= 0) {
    nextStepAt = now + stepMs;
    const int16_t N = (int16_t)gauge.numPixels();

    // advance + bounce
    cursor += dir;
    if (cursor <= 0)       { cursor = 0;       dir = +1; }
    else if (cursor >= N-1){ cursor = N-1;     dir = -1; }

    drawRunning(now);
  }

  // One-try RFID detection (rising edge)
  bool present = isAnyCardPresent(rfid);
  if (!tried && present && !tagPrev) {
    TrexUid uid{};
    if (readUid(rfid, uid)) {
      tried = true;
      triedUid = uid;

      const bool success = inSeg((uint16_t)cursor);

      // Freeze cursor + outcome visuals
      if (success) {
        st = MgState::Success;
        drawSuccess();
        startLootAudio(true); // bonus one-shot
      } else {
        st = MgState::Miss;
        nextBlinkAt = now;    // blink immediately
        drawMiss(now);
      }

      // Report
      sendMgResult(uid, success ? 1 : 0);
    }
  }
  tagPrev = present;

  // Maintain miss blink
  if (st == MgState::Miss) {
    drawMiss(now);
  }
}
