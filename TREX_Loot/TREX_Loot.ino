/*
  TREX – Loot Station (Feather S3 + RC522 + MAX98357 + Ring + Gauge)
  ------------------------------------------------------------------
  • RC522              CS=5  RST=6   SPI: SCK=36 MISO=37 MOSI=35
  • 14-px ring         GPIO 38
  • 56-px pipe gauge   GPIO 18   (set GAUGE_LEN to your strip length)
  • MOSFET light       GPIO 17
  • I²S audio          BCLK 43 | LRCLK 44 | DOUT 33
  • Transport          ESP-NOW via TrexTransport (channel must match T-Rex)

  One-firmware identities:
  - STATION_ID and HOSTNAME now live in NVS (Preferences).
  - First boot: auto-assign ID = (EFUSE MAC LSB % 5) + 1 → HOSTNAME "Loot-<ID>".
  - You can override at any time via Serial:
      whoami
      id <1..5>
      host <name>
      ident <1..5> <name>
*/

// ======= AUDIO BACKEND SELECTOR =======
// 1 = PROGMEM (embed .wav in code)   |  0 = LittleFS (+ buffer)
#define TREX_AUDIO_PROGMEM 1
// =====================================

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_NeoPixel.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>

#include <cstring>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "esp_ota_ops.h"
#include "esp_system.h"

#if TREX_AUDIO_PROGMEM
  #include <AudioFileSourcePROGMEM.h>
  #include "replenish.h"
  #include "replenish_bonus.h"
  #include "bonus_spawn.h"      // <— NEW: the chime that plays when station enters bonus
  // Selected clip pointers (normal by default)
  static const uint8_t* g_clipData = replenish_wav;
  static size_t         g_clipLen  = replenish_wav_len;
#else
  #include <AudioFileSourceLittleFS.h>
  #include <AudioFileSourceBuffer.h>
  constexpr char CLIP_PATH[]        = "/replenish.wav";
  constexpr char CLIP_PATH_BONUS[]  = "/replenish_bonus.wav";
  constexpr char CLIP_PATH_SPAWN[]  = "/bonus_spawn.wav";  // <— NEW (FS path)
  static const char* g_clipPath = CLIP_PATH;
#endif

#include <TrexProtocol.h>
#include <TrexTransport.h>
#include "TrexMaintenance.h"      // ← maintenance mode (OTA, Telnet, mDNS, HTTP FS)
#include <TrexVersion.h>

/* ---------- Wi-Fi (Maintenance / OTA HTTP) ---------- */
#define WIFI_SSID   "GUD"
#define WIFI_PASS   "EscapE66"

bool transportReady = false;
bool otaSuccessReportPending = false;   // set if we find /ota.json=success at boot
uint32_t otaSuccessSendAt = 0;

/* ── IDs & radio ─────────────────────────────────────── */
constexpr uint8_t  WIFI_CHANNEL  = 6;        // must match the T-Rex server

// --- Persistent identity (NVS) ---
Preferences idstore;
uint8_t  STATION_ID = 0;                      // loaded from NVS
char     HOSTNAME[32] = "Loot-0";             // loaded from NVS

static void loadIdentity() {
  idstore.begin("trex", true);                // read-only
  STATION_ID = idstore.getUChar("id", 0);
  String h   = idstore.getString("host", "Loot-0");
  idstore.end();
  strlcpy(HOSTNAME, h.c_str(), sizeof(HOSTNAME));
}

static void saveIdentity(uint8_t id, const char* host) {
  idstore.begin("trex", false);
  idstore.putUChar("id", id);
  idstore.putString("host", host);
  idstore.end();
}

static void ensureIdentity() {
  loadIdentity();
  if (STATION_ID == 0 || HOSTNAME[0] == '\0' || !strncmp(HOSTNAME, "Loot-0", 6)) {
    // Derive a stable default from EFUSE MAC: map to 1..5
    uint8_t derived = (uint8_t)(ESP.getEfuseMac() & 0xFF);
    uint8_t id = (derived % 5) + 1;
    char host[32];
    snprintf(host, sizeof(host), "Loot-%u", id);
    saveIdentity(id, host);
    loadIdentity();
    Serial.printf("[ID] Auto-provisioned -> id=%u host=%s (from EFUSE MAC)\n", STATION_ID, HOSTNAME);
  } else {
    Serial.printf("[ID] Loaded -> id=%u host=%s\n", STATION_ID, HOSTNAME);
  }
}

/* ── LED config ──────────────────────────────────────── */
constexpr uint16_t GAUGE_LEN        = 56;     // pipe gauge length (px)
constexpr uint8_t  RING_BRIGHTNESS  = 64;
constexpr uint8_t  GAUGE_BRIGHTNESS = 255;
// Put this near your LED drawing helpers:
constexpr uint8_t RING_ROTATE = 0; // adjust if index 0 isn’t your physical “LED 1”

// LED RING Pair order: (1,2) → (14,3) → (13,4) → (12,5) → (11,6) → (10,7) → (9,8)
// (0-based indexes for the library)
static const uint8_t ORDER_SYM_14[14] PROGMEM = {
  0, 1, 13, 2, 12, 3, 11, 4, 10, 5, 9, 6, 8, 7
};

/* ── pins ────────────────────────────────────────────── */
constexpr uint8_t PIN_SCK      = 36;
constexpr uint8_t PIN_MISO     = 37;
constexpr uint8_t PIN_MOSI     = 35;
constexpr uint8_t PIN_RFID_CS  = 5;
constexpr uint8_t PIN_RFID_RST = 6;

constexpr uint8_t PIN_RING     = 38;
constexpr uint8_t PIN_GAUGE    = 18;
constexpr uint8_t PIN_MOSFET   = 17;

constexpr int PIN_I2S_BCLK     = 43;
constexpr int PIN_I2S_LRCLK    = 44;
constexpr int PIN_I2S_DOUT     = 33;

/* ── objects ─────────────────────────────────────────── */
MFRC522 rfid(PIN_RFID_CS, PIN_RFID_RST);
Adafruit_NeoPixel ring (14,         PIN_RING,  NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel gauge(GAUGE_LEN,  PIN_GAUGE, NEO_GRB + NEO_KHZ800);

// --- audio (conditional) ---
#if TREX_AUDIO_PROGMEM
  // In-place storage + pointer (no heap allocations)
  alignas(4) static uint8_t wavSrcStorage[sizeof(AudioFileSourcePROGMEM)];
  static AudioFileSourcePROGMEM* wavSrc = nullptr;
#else
  AudioFileSourceLittleFS *wavFile = nullptr;
  AudioFileSourceBuffer   *wavBuf  = nullptr;
#endif

AudioGeneratorWAV *decoder = nullptr;
AudioOutputI2S    *i2sOut  = nullptr;
bool               playing = false;

/* ── colours ─────────────────────────────────────────── */
static inline uint32_t C_RGB(uint8_t r,uint8_t g,uint8_t b){ return Adafruit_NeoPixel::Color(r,g,b); }
const uint32_t RED     = C_RGB(255,0,0);
const uint32_t GREEN   = C_RGB(0,255,0);
const uint32_t BLUE    = C_RGB(0,0,255);
const uint32_t CYAN    = C_RGB(0,200,255);
const uint32_t YELLOW    = C_RGB(255,180,0);
const uint32_t WHITE   = C_RGB(255,255,255);
const uint32_t OFF     = 0;

static bool ringCarriedValid = false;

/* ── RFID presence debounce ──────────────────────────── */
constexpr uint32_t ABSENCE_MS = 150;   // debounce removal
bool          tagPresent      = false;
uint32_t      absentStartMs   = 0;

// ── "Full bracelet" ring blink ─────────────────────────
constexpr uint16_t FULL_BLINK_PERIOD_MS = 320;  // ~3 Hz
bool     fullBlinkActive = false;
bool     fullBlinkOn     = false;
uint32_t fullBlinkLastMs = 0;
uint32_t blinkHoldId     = 0;       // which hold this blink belongs to

// ── Yellow gauge blink (500 ms) ───────────────────────
constexpr uint16_t YELLOW_BLINK_PERIOD_MS = 500;
bool     yellowBlinkActive = false;
bool     yellowBlinkOn     = false;
uint32_t yellowBlinkLastMs = 0;

// ── Empty-station blink (first gauge LED white) ─────────────
constexpr uint16_t EMPTY_BLINK_PERIOD_MS = 500;
bool     emptyBlinkActive = false;
bool     emptyBlinkOn     = false;
uint32_t emptyBlinkLastMs = 0;

// Bonus rainbow animation state (client-side)
static uint16_t g_rainbowPhase = 0;               // 0..65535 hue offset
constexpr uint16_t RAINBOW_STEP    = 768;         // hue advance per frame (~fast smooth)
constexpr uint16_t RAINBOW_FRAME_MS= 33;          // ~30 FPS

// ── Bonus warning blink (last 3s of intermission) ──────────────────────────
constexpr uint32_t BONUS_WARN_MS         = 3000;  // blink when msLeft <= this
constexpr uint32_t BONUS_BLINK_PERIOD_MS = 220;   // ~4.5 Hz

static bool     bonusBlinkActive   = false;
static bool     bonusBlinkPhaseOn  = true;

inline void startBonusBlink() { /* disabled */ }
inline void stopBonusBlink()  { bonusBlinkActive = false; }
inline void tickBonusBlink()  { /* disabled */ }

/* ── Game/hold state (server-auth) ───────────────────── */
volatile bool gameActive = true;     // flipped by GAME_OVER/START in onRx()
volatile bool holdActive = false;    // true only after accepted ACK
bool      wasPaused  = false;        // local pause latch

uint32_t  holdId       = 0;
uint8_t   carried      = 0;
uint8_t   maxCarry     = 8;
uint16_t  inv          = 0;
uint16_t  cap          = GAUGE_LEN;
TrexUid   currentUid{};

LightState g_lightState = LightState::GREEN;

// Gauge paint cache (prevents unnecessary .show() calls)
uint16_t   lastInvPainted = 0, lastCapPainted = 0;
LightState lastGaugeColor = LightState::GREEN;
static bool gaugeCacheValid = false;

static bool     fullAnnounced     = false;
static uint32_t nextGaugeDrawAtMs = 0;

/* ── misc ────────────────────────────────────────────── */
uint16_t g_seq = 1;            // outgoing message seq

// Set true once we’ve received the first STATION_UPDATE/ACK/TICK for this game
static bool stationInited = false;

static bool s_isBonusNow = false;
static bool g_audioOneShot = false;
static uint32_t g_bonusExclusiveUntilMs = 0;
static bool     g_chimeActive = false;
static bool     g_resumeLoopAfterChime = false;   // resume replenish loop after chime

/* ── OTA campaign state ──────────────────────────────── */
bool      otaInProgress     = false;
bool      otaStartRequested = false;
uint32_t  otaCampaignId     = 0;
uint8_t   otaExpectMajor    = 0, otaExpectMinor = 0;
char      otaUrl[128]       = {0};

// Spinner/progress visuals
bool      otaSpinnerActive  = false;
uint8_t   otaSpinnerIdx     = 0;
uint32_t  otaSpinnerLastMs  = 0;
constexpr uint16_t OTA_SPINNER_MS = 60;

// ── Soft staggering to avoid same-ms spikes ──────────────────────────────
// Tweak these if you like; they just separate events in time a bit.
constexpr uint16_t RING_STAGGER_MS   = 0;   // ring can stay at 0 (baseline)
constexpr uint16_t EMPTY_STAGGER_MS  = 70;  // empty-gauge blink toggles 70ms after ring
constexpr uint16_t AUDIO_STOP_STAGGER_MS = 12; // stop audio slightly after a FULL ring starts

// ── Round 4.5 mini-game (local) ────────────────────────
static bool     r45Active   = false;
static bool     r45Used     = false;   // one try per station
static bool     r45Success  = false;

static uint8_t  r45SegStart = 0;       // random segment [start, start+len-1]
static uint8_t  r45SegLen   = 10;
static uint16_t r45StepMs   = 30;      // ms per pixel step
static uint8_t  r45Pos      = 0;       // green LED pos 0..GAUGE_LEN-1
static int8_t   r45Dir      = +1;      // +1/-1 bounce
static uint32_t r45NextStepAt = 0;

static const uint16_t R45_HOLD_MS = 200; // must hold inside segment this long
static bool     r45Attempting = false;
static uint32_t r45AttemptStart = 0;
static uint8_t  r45FreezePos = 0;      // where we freeze on miss

// ---- Serial identity commands (whoami / id / host / ident) ----
static void processIdentitySerial() {
  static char buf[96]; static size_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[len] = 0;
      if (strcmp(buf, "whoami") == 0) {
        Serial.printf("[ID] id=%u host=%s\n", STATION_ID, HOSTNAME);
      } else if (!strncmp(buf, "id ", 3)) {
        int id = atoi(buf+3);
        if (id >= 1 && id <= 5) {
          saveIdentity((uint8_t)id, HOSTNAME);
          Serial.printf("[ID] Saved id=%d (host=%s). Rebooting…\n", id, HOSTNAME);
          delay(200); ESP.restart();
        } else {
          Serial.println("[ID] Usage: id <1..5>");
        }
      } else if (!strncmp(buf, "host ", 5)) {
        const char* h = buf+5;
        if (*h) {
          saveIdentity(STATION_ID, h);
          Serial.printf("[ID] Saved host=%s (id=%u). Rebooting…\n", h, STATION_ID);
          delay(200); ESP.restart();
        } else Serial.println("[ID] Usage: host <name>");
      } else if (!strncmp(buf, "ident ", 6)) {
        int id = 0; char name[32] = {0};
        if (sscanf(buf+6, "%d %31s", &id, name) == 2 && id>=1 && id<=5) {
          saveIdentity((uint8_t)id, name);
          Serial.printf("[ID] Saved id=%d host=%s. Rebooting…\n", id, name);
          delay(200); ESP.restart();
        } else {
          Serial.println("[ID] Usage: ident <1..5> <name>");
        }
      } else if (len) {
        Serial.println("[ID] cmds: whoami | id <1..5> | host <name> | ident <1..5> <name>");
      }
      len = 0;  // reset buffer
      continue;
    }
    if ((c == 8 || c == 127) && len > 0) { len--; continue; }  // backspace
    if (len < sizeof(buf)-1) buf[len++] = c;
  }
}

/* ── audio helpers ────────────────────────────────────── */
static bool openChain() {
  if (decoder && decoder->isRunning()) decoder->stop();

#if TREX_AUDIO_PROGMEM
  // Use the currently selected PROGMEM clip
  wavSrc = new (wavSrcStorage) AudioFileSourcePROGMEM(g_clipData, g_clipLen);
#else
  // Use the currently selected LittleFS path
  if (wavBuf)  { delete wavBuf;  wavBuf  = nullptr; }
  if (wavFile) { delete wavFile; wavFile = nullptr; }
  wavFile = new AudioFileSourceLittleFS(g_clipPath);
  if (!wavFile) { Serial.println("[LOOT] wavFile alloc fail"); return false; }
  wavBuf = new AudioFileSourceBuffer(wavFile, 4096);
  if (!wavBuf) { Serial.println("[LOOT] wavBuf alloc fail"); return false; }
#endif

  // Fresh decoder each time avoids stale state
  if (decoder) { decoder->stop(); delete decoder; decoder = nullptr; }
  decoder = new AudioGeneratorWAV();

#if TREX_AUDIO_PROGMEM
  bool ok = decoder->begin(wavSrc, i2sOut);
#else
  bool ok = decoder->begin(wavBuf, i2sOut);
#endif
  if (!ok) Serial.println("[LOOT] decoder.begin() failed");
  return ok;
}

bool startAudio() { if (playing) return true; playing = openChain(); return playing; }

void stopAudio() {
  if (!playing) return;
  decoder->stop();        // clean stop so next begin() works
  playing = false;
}

inline void handleAudio() {
  if (!playing || !decoder) return;
  if (!decoder->loop()) {                 // EOF or starvation
    decoder->stop();
    if (g_audioOneShot || g_chimeActive) {
      // one-shot or chime finished
      playing = false;
      // For spawn chime, don't resume replenish (we just ended the hold)
      if (g_chimeActive) {
        g_chimeActive = false;
        g_audioOneShot = false;
        g_resumeLoopAfterChime = false;
      } else {
        // regular one-shot (e.g., bonus loop one-shot if you ever use it)
        g_audioOneShot = false;
        if (g_resumeLoopAfterChime && holdActive) {
          g_resumeLoopAfterChime = false;
          selectClip(false);             // normal replenish loop
          startAudio();
        }
      }
    } else {
      // normal replenish loop: keep looping
      playing = openChain();
      if (!playing) Serial.println("[LOOT] audio re-begin failed");
    }
  }
}

static inline void selectClip(bool bonus) {
#if TREX_AUDIO_PROGMEM
  if (bonus) { g_clipData = replenish_bonus_wav; g_clipLen = replenish_bonus_wav_len; }
  else        { g_clipData = replenish_wav;      g_clipLen = replenish_wav_len; }
#else
  g_clipPath = bonus ? CLIP_PATH_BONUS : CLIP_PATH;
#endif
}

static inline bool startLootAudio(bool bonus) {
  g_audioOneShot = bonus;         // tap = one-shot
  if (bonus) g_bonusExclusiveUntilMs = millis() + 350;   // first 350 ms: be gentle
  else       g_bonusExclusiveUntilMs = 0;
  selectClip(bonus);
  return startAudio();
}

// Pre-empt replenish loop and play spawn chime fully, even if HOLD_END arrives.
// Do NOT resume the loop afterwards (round just switched behavior).
static void playBonusSpawnChime() {
  if (g_chimeActive) return;             // already playing a chime
  bool wasLooping = playing && !g_audioOneShot;

  if (wasLooping) {
    stopAudio();                         // cleanly stop replenish loop
    g_resumeLoopAfterChime = false;      // do not resume after chime
  }

  g_chimeActive  = true;
  g_audioOneShot = true;                  // ensures we don't auto-loop

#if TREX_AUDIO_PROGMEM
  const uint8_t* savedData = g_clipData; size_t savedLen = g_clipLen;
  g_clipData = bonus_spawn_wav; g_clipLen = bonus_spawn_wav_len;
  startAudio();
  g_clipData = savedData; g_clipLen = savedLen;
#else
  const char* savedPath = g_clipPath;
  g_clipPath = CLIP_PATH_SPAWN;
  startAudio();
  g_clipPath = savedPath;
#endif
}

/* ── helpers ─────────────────────────────────────────── */
bool isAnyCardPresent(MFRC522 &m) {
  byte atqa[2]; byte len = 2;
  return m.PICC_WakeupA(atqa, &len) == MFRC522::STATUS_OK;
}
bool readUid(MFRC522 &m, TrexUid &out) {
  if (!m.PICC_ReadCardSerial()) return false;
  out.len = m.uid.size;
  for (uint8_t i=0; i<out.len && i<10; ++i) out.bytes[i] = m.uid.uidByte[i];
  return true;
}

void packHeader(uint8_t type, uint16_t payLen, uint8_t* buf) {
  auto* h = (MsgHeader*)buf;
  h->version      = TREX_PROTO_VERSION;
  h->type         = type;
  h->srcStationId = STATION_ID;    // ← now runtime-loaded
  h->flags        = 0;
  h->payloadLen   = payLen;
  h->seq          = g_seq++;
}

// One-shot scheduled audio stop
static uint32_t schedAudioStopAt = 0;
inline void scheduleAudioStop(uint16_t delayMs) { schedAudioStopAt = millis() + delayMs; }
inline void tickScheduledAudio() {
  if (schedAudioStopAt && (int32_t)(millis() - schedAudioStopAt) >= 0) {
    stopAudio();
    schedAudioStopAt = 0;
  }
}

/* ── LED drawing ─────────────────────────────────────── */
inline uint32_t gaugeColor() {
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
      uint32_t c = gauge.ColorHSV(hue, /*sat*/255, /*val*/255);   // full saturation/brightness
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
inline void drawGaugeAuto(uint16_t inventory, uint16_t capacity) {
  if (s_isBonusNow && inventory > 0 && g_lightState == LightState::GREEN) {
    drawGaugeInventoryRainbowAnimated(inventory, capacity, g_rainbowPhase);
  } else {
    drawGaugeInventory(inventory, capacity);
  }
}

inline void tickBonusRainbow() {
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

inline void startFullBlinkImmediate() {
  fullBlinkActive = true;
  fullBlinkOn     = true;
  fullBlinkLastMs = millis();
  blinkHoldId     = holdId;
  fillRing(YELLOW);
}
inline void stopFullBlink() { fullBlinkActive = false; fullBlinkOn = false; }
inline void tickFullBlink() {
  if (!fullBlinkActive) return;
  uint32_t now = millis();
  if ((now - fullBlinkLastMs) >= FULL_BLINK_PERIOD_MS) {
    fullBlinkLastMs = now;
    fullBlinkOn = !fullBlinkOn;
    fillRing(fullBlinkOn ? YELLOW : OFF);
  } else {                                   // NEW: drip-feed between flips
  }
}

void gameOverBlinkAndOff() {
  // 3 quick red blinks on ring + gauge + MOSFET, then off
  const int cycles = 3;
  for (int k = 0; k < cycles; ++k) {
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

inline void startYellowBlinkImmediate() {
  yellowBlinkActive = true;
  yellowBlinkOn     = true;
  yellowBlinkLastMs = millis() + RING_STAGGER_MS;  // ← stagger start
  // Paint immediately in current color (YELLOW) at current inv level
  drawGaugeInventory(inv, cap);
}

inline void stopYellowBlink() {
  yellowBlinkActive = false;
  yellowBlinkOn = false;
}

inline void tickYellowBlink() {
  if (!yellowBlinkActive) return;
  uint32_t now = millis();
  if ((now - yellowBlinkLastMs) >= YELLOW_BLINK_PERIOD_MS) {
    yellowBlinkLastMs = now;
    yellowBlinkOn = !yellowBlinkOn;
    if (yellowBlinkOn) {
      // ON phase: draw lit portion in YELLOW (via drawGaugeInventory)
      drawGaugeInventory(inv, cap);
    } else {
      // OFF phase: darken the gauge (don’t affect MOSFET)
      fillGauge(OFF);
    }
  }
}

// ── Empty-station blink (first gauge LED white) ─────────────
inline void forceGaugeRepaint() {
  gaugeCacheValid = false;         // bust cache so we truly repaint
  drawGaugeAuto(inv, cap);         // rainbow if bonus
}

inline void applyEmptyOverlay() {
  // Overlay only if actively scanning an empty station (and not OTA)
  if (!emptyBlinkActive || otaInProgress || !tagPresent || inv != 0) return;
  // Optional guard to respect YELLOW off-phase:
  if (g_lightState == LightState::YELLOW && yellowBlinkActive && !yellowBlinkOn) return;
  // Paint just LED 0: white when ON, off when OFF
  gauge.setPixelColor(0, emptyBlinkOn ? WHITE : OFF);
  gauge.show();
}

// Empty gauge blink: stop audio once when the blink begins, then blink as usual.
inline void startEmptyBlink() {
  emptyBlinkActive = true;
  emptyBlinkOn     = true;
  emptyBlinkLastMs = millis() + EMPTY_STAGGER_MS;  // ← stagger start
  forceGaugeRepaint();                             // keep your immediate visual
}

inline void stopEmptyBlink() {
  if (!emptyBlinkActive) return;
  emptyBlinkActive = false;
  emptyBlinkOn     = false;
  forceGaugeRepaint();
}

inline void tickEmptyBlink() {
  if (!emptyBlinkActive) return;
  uint32_t now = millis();
  if ((now - emptyBlinkLastMs) >= EMPTY_BLINK_PERIOD_MS) {
    emptyBlinkLastMs = now;
    emptyBlinkOn = !emptyBlinkOn;
    forceGaugeRepaint();
  } else {
  }
}

// Paint only when we’re not in an OFF phase of Yellow blink (bonus blink removed)
inline bool canPaintGaugeNow() {
  return (!yellowBlinkActive || yellowBlinkOn);
}

inline bool r45Inside(uint8_t pos) {
  return (pos >= r45SegStart) && (pos < (uint8_t)(r45SegStart + r45SegLen));
}

void drawR45Frame() {
  // Base: OFF
  for (uint16_t i=0;i<GAUGE_LEN;++i) gauge.setPixelColor(i, 0);

  // Rainbow segment
  for (uint16_t i=0;i<r45SegLen;++i) {
    uint16_t idx = (uint16_t)(r45SegStart + i);
    if (idx < GAUGE_LEN) {
      uint32_t hue = (uint32_t)((i * 65535u) / (r45SegLen ? r45SegLen : 1));
      gauge.setPixelColor(idx, gauge.ColorHSV((uint16_t)hue, 255, 255));
    }
  }

  // Green LED (moving or frozen)
  uint8_t gp = r45Used && !r45Success ? r45FreezePos : r45Pos;
  if (gp < GAUGE_LEN) gauge.setPixelColor(gp, GREEN);

  gauge.show();
}

/* ── OTA visuals ─────────────────────────────────────── */
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

/* ── OTA status send ─────────────────────────────────── */
void sendOtaStatus(OtaPhase phase, uint8_t errCode, uint32_t bytes, uint32_t total) {
  if (otaInProgress) return;
  if (!transportReady) { return; }
  uint8_t buf[sizeof(MsgHeader)+sizeof(OtaStatusPayload)];
  packHeader((uint8_t)MsgType::OTA_STATUS, sizeof(OtaStatusPayload), buf);
  auto* p = (OtaStatusPayload*)(buf + sizeof(MsgHeader));
  p->stationType = (uint8_t)StationType::LOOT;
  p->stationId   = STATION_ID;
  p->campaignId  = otaCampaignId;
  p->phase       = (uint8_t)phase;
  p->error       = errCode;
  p->fwMajor     = TREX_FW_MAJOR;  p->fwMinor = TREX_FW_MINOR;   // ← bump when you release
  p->bytes       = bytes;
  p->total       = total;
  Transport::sendToServer(buf, sizeof(buf));
}

/* ── OTA persistence (/ota.json) ─────────────────────── */
void otaWriteFile(bool successPending) {
  StaticJsonDocument<128> d;
  d["campaignId"] = otaCampaignId;
  d["successPending"] = successPending ? 1 : 0;
  File f = LittleFS.open("/ota.json", "w");
  if (!f) return;
  serializeJson(d, f);
  f.close();
}

bool otaReadFile(uint32_t &campId, bool &successPending) {
  File f = LittleFS.open("/ota.json", "r");
  if (!f) return false;
  StaticJsonDocument<128> d;
  if (deserializeJson(d, f)) { f.close(); return false; }
  f.close();
  campId = d["campaignId"] | 0;
  successPending = (int)(d["successPending"] | 0) != 0;
  return true;
}

void otaClearFile() { LittleFS.remove("/ota.json"); }

/* ── Do the OTA (blocking in STA) ────────────────────── */
bool doOtaFromUrlDetailed(const char* url) {
  Serial.printf("[OTA] URL: %s\n", url);
  sendOtaStatus(OtaPhase::STARTING, 0, 0, 0);   // will be muted if otaInProgress guard is active

  // ---- Join Wi-Fi (STA) ----
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 15000) {
      Serial.println("[OTA] WiFi connect timeout");
      sendOtaStatus(OtaPhase::FAIL, 1, 0, 0);
      otaVisualFail();
      WiFi.disconnect(true,true);
      WiFi.mode(WIFI_OFF);
      return false;
    }
    otaTickSpinner();
    delay(100);
  }
  Serial.printf("[OTA] WiFi connected: ch=%d  ip=%s\n",
                WiFi.channel(), WiFi.localIP().toString().c_str());

  // ---- HTTP request (no keep-alive) ----
  HTTPClient http;
  WiFiClient client;
  http.setReuse(false);       // force Connection: close
  http.setTimeout(15000);     // 15s read timeout

  if (!http.begin(client, url)) {
    Serial.println("[OTA] http.begin failed");
    sendOtaStatus(OtaPhase::FAIL, 2, 0, 0);
    otaVisualFail();
    WiFi.disconnect(true,true);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[OTA] HTTP code %d\n", code);
    sendOtaStatus(OtaPhase::FAIL, 2, 0, 0);
    http.end();
    otaVisualFail();
    WiFi.disconnect(true,true);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  int total = http.getSize();         // -1 if server didn’t send Content-Length
  Serial.printf("[OTA] total bytes: %d\n", total);

  // ---- Begin flash ----
  if (total > 0) {
    if (!Update.begin(total)) {
      Serial.printf("[OTA] Update.begin failed: %s (need %d)\n", Update.errorString(), total);
      sendOtaStatus(OtaPhase::FAIL, 4, 0, total);
      http.end();
      otaVisualFail();
      WiFi.disconnect(true,true);
      WiFi.mode(WIFI_OFF);
      return false;
    }
  } else {
    if (!Update.begin()) {
      Serial.printf("[OTA] Update.begin failed (no len): %s\n", Update.errorString());
      sendOtaStatus(OtaPhase::FAIL, 4, 0, 0);
      http.end();
      otaVisualFail();
      WiFi.disconnect(true,true);
      WiFi.mode(WIFI_OFF);
      return false;
    }
  }

  WiFiClient* stream = http.getStreamPtr();
  const size_t BUF = 2048;
  uint8_t buf[BUF];
  uint32_t got = 0;
  uint32_t lastActivity = millis();
  uint32_t lastDraw = 0;

  // Read until we’ve received Content-Length (or EOF if none), with inactivity timeout.
  while ( (total < 0) || (got < (uint32_t)total) ) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = (avail > BUF) ? BUF : avail;
      int read = stream->readBytes((char*)buf, toRead);
      if (read <= 0) { delay(1); continue; }

      size_t wrote = Update.write(buf, (size_t)read);
      if (wrote != (size_t)read) {
        Serial.printf("[OTA] Write error: %s at %lu/%d\n",
                      Update.errorString(), (unsigned long)got, total);
        sendOtaStatus(OtaPhase::FAIL, 4, got, (uint32_t)(total>0?total:0));
        http.end();
        otaVisualFail();
        WiFi.disconnect(true,true);
        WiFi.mode(WIFI_OFF);
        return false;
      }

      got += wrote;
      lastActivity = millis();

      // Smooth scheduler/Wi-Fi
      delay(0);

      // Draw progress (throttle to every 16 KB)
      if (total > 0 && (got - lastDraw) >= 16384) {
        otaDrawProgress(got, (uint32_t)total);
        lastDraw = got;
      }
    } else {
      // No data available right now
      otaTickSpinner();
      delay(1);

      // If we got a length and reached it, we’re done
      if (total > 0 && got >= (uint32_t)total) break;

      // If no length, consider EOF when socket closes
      if (total < 0 && !stream->connected() && stream->available() == 0) break;

      // Inactivity timeout
      if (millis() - lastActivity > 15000) {
        Serial.println("[OTA] Stream timeout (no data for 15s)");
        sendOtaStatus(OtaPhase::FAIL, 2, got, (uint32_t)(total>0?total:0));
        http.end();
        otaVisualFail();
        WiFi.disconnect(true,true);
        WiFi.mode(WIFI_OFF);
        return false;
      }
    }
  }

  // Finish & verify
  bool ok = Update.end(true);   // true = perform MD5/verify if supported
  http.end();

  if (!ok || !Update.isFinished()) {
    Serial.printf("[OTA] End/verify error: %s (wrote %lu/%d)\n",
                  Update.errorString(), (unsigned long)got, total);
    sendOtaStatus(OtaPhase::FAIL, 5, got, (uint32_t)(total>0?total:0));
    otaVisualFail();
    WiFi.disconnect(true,true);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  // Success → persist flag and reboot; SUCCESS will be reported after ESPNOW is up
  otaWriteFile(true);
  otaVisualSuccess();
  delay(200);
  ESP.restart();
  return true;  // not reached
}

/* ── NET: messages ───────────────────────────────────── */
void sendHello() {
  uint8_t buf[sizeof(MsgHeader)+sizeof(HelloPayload)];
  packHeader((uint8_t)MsgType::HELLO, sizeof(HelloPayload), buf);
  auto* p = (HelloPayload*)(buf + sizeof(MsgHeader));
  p->stationType = (uint8_t)StationType::LOOT;
  p->stationId   = STATION_ID;
  p->fwMajor = TREX_FW_MAJOR; p->fwMinor = TREX_FW_MINOR;      // ← bump when you release
  p->wifiChannel = WIFI_CHANNEL;
  memset(p->mac, 0, 6);
  Transport::sendToServer(buf, sizeof(buf));
}

void sendHoldStart(const TrexUid& uid) {
  uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldStartPayload)];
  holdId = (uint32_t)esp_random();
  packHeader((uint8_t)MsgType::LOOT_HOLD_START, sizeof(LootHoldStartPayload), buf);
  auto* p = (LootHoldStartPayload*)(buf + sizeof(MsgHeader));
  p->holdId = holdId; p->uid = uid; p->stationId = STATION_ID;
  Transport::sendToServer(buf, sizeof(buf));
}

void sendHoldStop() {
  if (!holdId) return;
  uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldStopPayload)];
  packHeader((uint8_t)MsgType::LOOT_HOLD_STOP, sizeof(LootHoldStopPayload), buf);
  auto* p = (LootHoldStopPayload*)(buf + sizeof(MsgHeader));
  p->holdId = holdId;
  Transport::sendToServer(buf, sizeof(buf));
  holdActive = false;
  holdId = 0;
}

// sender
static void sendRound45Result(bool success, uint8_t greenPos, uint8_t segStart, uint8_t segLen) {
  if (!transportReady) return;
  uint8_t buf[sizeof(MsgHeader)+sizeof(Round45ResultPayload)];
  packHeader((uint8_t)MsgType::ROUND45_RESULT, sizeof(Round45ResultPayload), buf);
  auto* p = (Round45ResultPayload*)(buf + sizeof(MsgHeader));
  p->stationId = STATION_ID;
  p->success   = success ? 1 : 0;
  p->greenPos  = greenPos;
  p->segStart  = segStart;
  p->segLen    = segLen;
  Transport::sendToServer(buf, sizeof(buf));
}

/* ── RX handler ─────────────────────────────────────── */
void onRx(const uint8_t* data, uint16_t len) {
  if (len < sizeof(MsgHeader)) return;
  auto* h = (const MsgHeader*)data;
  if (h->version != TREX_PROTO_VERSION) {
    Serial.printf("[WARN] Proto mismatch on RX: got=%u exp=%u (type=%u)\n",
                  h->version, (unsigned)TREX_PROTO_VERSION, h->type);
    return;
  }

  switch ((MsgType)h->type) {
    case MsgType::STATE_TICK: {
      if (h->payloadLen < 1) break;
      const StateTickPayload* p =
          (const StateTickPayload*)(data + sizeof(MsgHeader));

      // Update light state
      if      (p->state == (uint8_t)LightState::GREEN)  g_lightState = LightState::GREEN;
      else if (p->state == (uint8_t)LightState::YELLOW) g_lightState = LightState::YELLOW;
      else                                              g_lightState = LightState::RED;

      // If we're in 4.5, don't paint the gauge here
      if (r45Active) break;

      // Arm/stop Yellow blink using your existing flags
      if (g_lightState == LightState::YELLOW) {
        yellowBlinkActive = true;   // tickYellowBlink() will manage phase
      } else {
        stopYellowBlink();
      }

      // After GAME_OVER keep gauge off
      if (!gameActive) {
        if (g_lightState == LightState::RED &&
            !holdActive && !otaInProgress) fillRing(RED);
        break;
      }

      // During game: draw gauge unless Yellow blink OFF phase
      if (stationInited && canPaintGaugeNow()) {
        drawGaugeAuto(inv, cap);
      }
      break;
    }

    case MsgType::ROUND45_START: {
      if (h->payloadLen != sizeof(Round45StartPayload)) break;
      auto* p = (const Round45StartPayload*)(data + sizeof(MsgHeader));

      // Enter 4.5 mode, reset attempt flags
      r45Active = true; r45Used = false; r45Success = false; r45Attempting = false;

      // Randomize segment and speed within windows
      uint8_t segMin = p->segMin, segMax = p->segMax;
      if (segMin < 1) segMin = 1;
      if (segMax < segMin) segMax = segMin;
      r45SegLen = segMin + (esp_random() % (uint32_t)(segMax - segMin + 1));
      if (r45SegLen > GAUGE_LEN) r45SegLen = GAUGE_LEN;
      if (r45SegLen < 1) r45SegLen = 1;

      uint8_t maxStart = (GAUGE_LEN > r45SegLen) ? (uint8_t)(GAUGE_LEN - r45SegLen) : 0;
      r45SegStart = (maxStart > 0) ? (uint8_t)(esp_random() % (maxStart + 1)) : 0;

      uint16_t sMin = p->stepMsMin, sMax = p->stepMsMax;
      if (sMin < 5) sMin = 5;
      if (sMax < sMin) sMax = sMin;
      r45StepMs     = sMin + (uint32_t)(esp_random() % (uint32_t)(sMax - sMin + 1));

      // Init green at 0 moving forward
      r45Pos = 0; r45Dir = +1; r45NextStepAt = millis() + r45StepMs;

      // Draw first frame
      drawR45Frame();
      break;
    }

    case MsgType::LOOT_HOLD_ACK: {
      if (h->payloadLen != sizeof(LootHoldAckPayload)) break;
      const LootHoldAckPayload* p =
          (const LootHoldAckPayload*)(data + sizeof(MsgHeader));
      if (p->holdId != holdId) break;

      if (r45Active) break; // ignore normal looting visuals during 4.5

      // Update state from ACK
      maxCarry = p->maxCarry;
      carried  = (p->carried > maxCarry) ? maxCarry : p->carried;  // clamp
      inv      = p->inventory;
      cap      = p->capacity;
      stationInited = true;

      // Keep/clear empty indicator based on latest inventory
      if (tagPresent && inv == 0) startEmptyBlink();
      else                        stopEmptyBlink();

      if (p->accepted) {
        if (!gameActive) break;   // if game ended mid-flight, ignore visuals

        holdActive = true;

        startLootAudio(s_isBonusNow);

        // FULL indicator (ring blink) vs carried ring
        if (carried >= maxCarry) {
          if (!fullAnnounced || blinkHoldId != p->holdId) {
            startFullBlinkImmediate();      // (has chime guard, or add if you like)
            fullAnnounced = true;
            blinkHoldId   = p->holdId;
            scheduleAudioStop(AUDIO_STOP_STAGGER_MS);   // ← stop audio a few ms later
          }
        } else {
          if (fullBlinkActive) stopFullBlink();
          fullAnnounced = false;
          drawRingCarried(carried, maxCarry);
        }

        // Throttled gauge repaint first (so LED frame doesn't coincide with audio begin)
        uint32_t now = millis();
        if ((int32_t)(now - nextGaugeDrawAtMs) >= 0 && canPaintGaugeNow()) {
          drawGaugeAuto(inv, cap);
          nextGaugeDrawAtMs = now + 20;
        }
      } else { // denied (FULL / EMPTY / EDGE_GRACE / RED, etc.)
        holdActive = false;
        // NOTE: do NOT stop audio here globally — only stop if FULL blink actually starts.

        if (carried >= maxCarry) {
          // FULL — ring blink + stop audio only on rising edge (inside startFullBlinkImmediate)
          if (!fullAnnounced || blinkHoldId != p->holdId) {
            startFullBlinkImmediate();
            fullAnnounced = true;
            blinkHoldId   = p->holdId;
            scheduleAudioStop(AUDIO_STOP_STAGGER_MS);   // ← stop audio a few ms later
          }
          // Defer gauge repaint; normal updates will paint soon
        } else {
          fullAnnounced = false;
          stopFullBlink();
          fillRing(RED);

          // Reflect inventory now (even if tag still present), but only if game still active
          uint32_t now = millis();
          if (gameActive && (int32_t)(now - nextGaugeDrawAtMs) >= 0 && canPaintGaugeNow()) {
            drawGaugeAuto(inv, cap);
            nextGaugeDrawAtMs = now + 20;
          }
        }
      }
      break;
    }

    case MsgType::LOOT_TICK: {
      if (h->payloadLen != sizeof(LootTickPayload)) break;
      auto* p = (const LootTickPayload*)(data + sizeof(MsgHeader));
      if (!holdActive || p->holdId != holdId) break;

      if (r45Active) break; // ignore normal looting visuals during 4.5

      carried = (p->carried > maxCarry) ? maxCarry : p->carried;  // clamp
      inv     = p->inventory;
      stationInited = true;

      // Maintain empty indicator
      if (tagPresent && inv == 0) startEmptyBlink();
      else                        stopEmptyBlink();

      // Full handling + ring update
      if (carried >= maxCarry) {
        if (!fullAnnounced || blinkHoldId != p->holdId) {
          startFullBlinkImmediate();
          fullAnnounced = true;
          blinkHoldId   = p->holdId;
          scheduleAudioStop(AUDIO_STOP_STAGGER_MS);   // ← stop audio a few ms later
        }
      } else {
        if (fullBlinkActive) stopFullBlink();
        fullAnnounced = false;
        drawRingCarried(carried, maxCarry);
      }

      // Throttled gauge render — GREEN shows rainbow automatically, YELLOW/RED override
      uint32_t now = millis();
      if ((int32_t)(now - nextGaugeDrawAtMs) >= 0) {
        drawGaugeAuto(inv, cap);
        nextGaugeDrawAtMs = now + 20;
      }

      break;
    }

    case MsgType::HOLD_END: {
      if (h->payloadLen != sizeof(HoldEndPayload)) break;
      auto* p = (const HoldEndPayload*)(data + sizeof(MsgHeader));
      if (p->holdId != holdId) break;

      stopEmptyBlink();

      holdActive = false;
      holdId     = 0;

      // Do NOT stop audio if the spawn chime is playing
      if (!g_audioOneShot && !g_chimeActive) {
        stopAudio();
      }

      fullAnnounced = false;

      if (tagPresent && carried >= maxCarry) {
        if (!fullBlinkActive) startFullBlinkImmediate();
      } else {
        stopFullBlink();
        fillRing(RED);
      }
      break;
    }

    case MsgType::STATION_UPDATE: {
      if (h->payloadLen != sizeof(StationUpdatePayload)) break;
      const StationUpdatePayload* p =
          (const StationUpdatePayload*)(data + sizeof(MsgHeader));
      if (p->stationId != STATION_ID) break;

      // Cache latest values
      inv = p->inventory;
      cap = p->capacity;
      stationInited = true;

      // In 4.5, do NOT repaint from inventory (always 0)
      if (r45Active) break;

      // Don't repaint after GAME_OVER
      if (!gameActive) break;

      if (!holdActive && !otaInProgress && canPaintGaugeNow()) {
        drawGaugeAuto(inv, cap);
      }
      break;
    }

    case MsgType::GAME_START: {
      gameActive = true;
      wasPaused = false;
      fullBlinkActive = false;
      fullAnnounced = false;

      stationInited = false;                  // wait for fresh inventory before painting
      gaugeCacheValid = false;

      digitalWrite(PIN_MOSFET, HIGH);         // lamp ON for live round
      stopFullBlink();
      stopEmptyBlink();
      fillRing(RED);                          // ring “awake” in RED

      // IMPORTANT: do NOT call drawGaugeInventory() here,
      // so we don’t blank to 0 before STATION_UPDATE arrives.
      Serial.println("[LOOT] GAME_START");
      break;
    }

    case MsgType::GAME_OVER: {
      if (len < sizeof(MsgHeader) + 1) break;

      const GameOverPayload* gp =
          (const GameOverPayload*)(data + sizeof(MsgHeader));

      const uint8_t reason   = (h->payloadLen >= 1) ? gp->reason : 0;
      const uint8_t blameSid = (h->payloadLen >= sizeof(GameOverPayload))
                              ? gp->blameSid : GAMEOVER_BLAME_ALL;

      // NEW: force local visuals/state to a safe post-game condition
      gameActive     = false;
      holdActive     = false;
      tagPresent     = false;
      fullBlinkActive= false;

      r45Active = false; r45Used = false; r45Attempting = false;

      // Leave bonus & stop last-3s blinker so it can't repaint the gauge
      s_isBonusNow = false;
      stopBonusBlink();

      // Lock light conceptually to RED so any later draws use red palette
      g_lightState = LightState::RED;

      stopYellowBlink();
      stopEmptyBlink();
      stopAudio();

      // Gauge OFF immediately; ring steady RED (targeted blink still allowed)
      fillGauge(OFF);
      if (!otaInProgress) fillRing(RED);

      // Your existing offender-based blink:
      const bool redViolation = (reason == 1); // RED_LOOT
      const bool offender     = redViolation &&
                                (blameSid != GAMEOVER_BLAME_ALL) &&
                                (blameSid == STATION_ID);
      const bool shouldBlink  = !redViolation || offender;
      if (shouldBlink) gameOverBlinkAndOff();

      Serial.printf("[LOOT] GAME_OVER reason=%u blame=%u me=%u\n",
                    reason, blameSid, STATION_ID);
      break;
    }

    case MsgType::CONFIG_UPDATE: {
      if (h->payloadLen != sizeof(ConfigUpdatePayload)) break;
      if (gameActive) { Serial.println("[OTA] Ignored (game active)"); break; }
      auto* p = (const ConfigUpdatePayload*)(data + sizeof(MsgHeader));

      // scope: LOOT + my id (or broadcast)
      bool typeMatch = (p->stationType == 0) || (p->stationType == (uint8_t)StationType::LOOT);
      bool idMatch   = (p->targetId == 0)    || (p->targetId == STATION_ID);
      if (!typeMatch || !idMatch) break;

      if (otaInProgress) { Serial.println("[OTA] Already in progress"); break; }
      if (p->otaUrl[0] == 0) { Serial.println("[OTA] No URL"); break; }

      // latch
      strncpy(otaUrl, p->otaUrl, sizeof(otaUrl)-1); otaUrl[sizeof(otaUrl)-1]=0;
      otaCampaignId  = p->campaignId;
      otaExpectMajor = p->expectMajor; otaExpectMinor = p->expectMinor;
      otaInProgress  = true;
      otaStartRequested = true;

      // immediate feedback + ACK
      Serial.printf("[OTA] CONFIG_UPDATE received, url=%s campaign=%lu\n", otaUrl, (unsigned long)otaCampaignId);
      break;
    }

    case MsgType::BONUS_UPDATE: {
      if (h->payloadLen < 4) break;
      const uint8_t* pl = data + sizeof(MsgHeader);

      uint32_t mask = (uint32_t)pl[0]
                    | ((uint32_t)pl[1] << 8)
                    | ((uint32_t)pl[2] << 16)
                    | ((uint32_t)pl[3] << 24);

      bool wasBonus = s_isBonusNow;
      s_isBonusNow  = ((mask >> STATION_ID) & 0x1u) != 0;

      if (!wasBonus && s_isBonusNow) {
        playBonusSpawnChime();              // your existing chime
      }
      if (wasBonus && !s_isBonusNow) {
        stopBonusBlink();                   // no-op, but keeps state clean
      }

      // Immediate repaint (rainbow in GREEN when bonus is on)
      if (gameActive && stationInited && !otaInProgress && canPaintGaugeNow()) {
        gaugeCacheValid = false;
        drawGaugeAuto(inv, cap);
      }
      break;
    }

    default: break;
  }
}

/* ── setup ───────────────────────────────────────────── */
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n[LOOT] Boot");

  // Log why we reset last time (brownout, WDT, etc.)
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("[BOOT] Reset reason: %d\n", (int)rr);

  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next    = esp_ota_get_next_update_partition(NULL);
  Serial.printf("[PART] running=%s size=%u\n", running? running->label : "?", running? running->size : 0);
  Serial.printf("[PART] next   =%s size=%u\n", next?    next->label    : "(none)", next?    next->size    : 0);

  // Provision identity (NVS); prints what it set/loaded
  ensureIdentity();

  // Always mount FS (for /ota.json even if audio is PROGMEM)
  if (!LittleFS.begin()) { LittleFS.begin(true); }

  // If we rebooted after OTA, announce SUCCESS once
  {
    uint32_t campId=0; bool success=false;
    if (otaReadFile(campId, success) && success) {
      otaCampaignId = campId;
      otaSuccessReportPending = true;          // defer report
      otaSuccessSendAt = millis() + 1200;      // wait ~1.2s after boot
    }
  }

  pinMode(PIN_MOSFET, OUTPUT);
  digitalWrite(PIN_MOSFET, HIGH);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);
  rfid.PCD_Init();

  ring.begin();  ring.setBrightness(RING_BRIGHTNESS);  fillRing(RED);
  gauge.begin(); gauge.setBrightness(GAUGE_BRIGHTNESS); gauge.clear(); gauge.show();

  i2sOut = new AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S);
  i2sOut->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT);
  i2sOut->SetGain(0.6f);

#if TREX_AUDIO_PROGMEM
  // First construction (open state = true)
  wavSrc = new (wavSrcStorage) AudioFileSourcePROGMEM(replenish_wav, replenish_wav_len);
#endif

#if !TREX_AUDIO_PROGMEM
  File f = LittleFS.open(CLIP_PATH, "r");
  if (!f) Serial.println("[LOOT] Missing /replenish.wav on LittleFS");
  else { Serial.printf("[LOOT] WAV size: %u bytes\n", (unsigned)f.size()); f.close(); }
#endif

  TransportConfig cfg{ /*maintenanceMode=*/false, /*wifiChannel=*/WIFI_CHANNEL };
  if (!Transport::init(cfg, onRx)) {
    Serial.println("[LOOT] Transport init FAILED");
    while (1) { delay(1000); }
  }

  transportReady = true;

  Serial.printf("Trex proto ver: %d\n", TREX_PROTO_VERSION);
  drawGaugeInventory(inv, cap);
}

/* ── loop ────────────────────────────────────────────── */
void loop() {
  // identity serial (non-blocking)
  processIdentitySerial();

  // ---- Runtime Maintenance Mode (long-press BOOT ~1.5 s) ----
  static Maint::Config mcfg{WIFI_SSID, WIFI_PASS, HOSTNAME,
                            /*apFallback=*/true, /*apChannel=*/WIFI_CHANNEL,
                            /*apPass=*/"trexsetup", /*buttonPin=*/0, /*holdMs=*/1500};
  // keep mcfg aligned with current identity
  mcfg.stationType  = StationType::LOOT;
  mcfg.stationId    = STATION_ID;
  mcfg.enableBeacon = true;

  // Maintenance checks
  if (Maint::checkRuntimeEntry(mcfg)) { fillRing(BLUE); fillGauge(BLUE); Maint::loop(); return; }
  if (Maint::active)                  { Maint::loop(); return; }

  // START OTA if requested (must come before any 'return' using otaInProgress)
  if (otaStartRequested) {
    otaStartRequested = false;

    otaVisualStart();                               // one-time cyan pulse
    // (No ACK anymore)

    Serial.printf("[OTA] Starting: %s\n", otaUrl);
    doOtaFromUrlDetailed(otaUrl);                   // blocking Wi-Fi download/flash

    // If we return here, OTA failed (success reboots from inside)
    otaInProgress = false;
    WiFi.disconnect(true,true);
    WiFi.mode(WIFI_OFF);
    fillRing(RED);
  }

  // While OTA runs, keep spinner and skip the rest of the logic
  if (otaInProgress) { otaTickSpinner(); return; }

  // Now normal networking
  Transport::loop();

  // Deferred SUCCESS (after ESPNOW is re-initialized)
  if (transportReady && otaSuccessReportPending && millis() >= otaSuccessSendAt) {
    sendOtaStatus(OtaPhase::SUCCESS, 0, 0, 0);
    otaClearFile();
    otaSuccessReportPending = false;
  }

  // ---- PAUSED / GAME OVER: only listen for messages ----
  if (!gameActive && !otaInProgress) {
    if (!wasPaused) {
      wasPaused = true;
      holdActive = false; holdId = 0; carried = 0;
      tagPresent = false; absentStartMs = 0;
      if (playing) stopAudio();
      fillRing(RED);
      stopYellowBlink();
      fillGauge(OFF);
      digitalWrite(PIN_MOSFET, LOW);
    }
    return;
  } else if (wasPaused) {
    wasPaused = false;
  }

  // ---- NORMAL ACTIVE LOOP ----
  const uint32_t now = millis();

  const bool present = isAnyCardPresent(rfid);

  // ARRIVAL
  if (present && !tagPresent) {
    if (readUid(rfid, currentUid)) {
      tagPresent    = true;
      absentStartMs = 0;
      carried       = 0;
      stopFullBlink();

      if (!r45Active) {
        // Normal behavior
        sendHoldStart(currentUid);
        if (inv == 0) startEmptyBlink(); else stopEmptyBlink();
      } else {
        // 4.5: handled in the r45Active loop above (no LOOT_HOLD_START)
        stopEmptyBlink();
      }
    }
  }

  // MAINTAINED
  if (present && tagPresent) { absentStartMs = 0; }

  // REMOVAL (debounced)
  if (!present && tagPresent) {
    if (absentStartMs == 0) absentStartMs = now;
    else if (now - absentStartMs > ABSENCE_MS) {
      tagPresent = false;
      absentStartMs = 0;
      sendHoldStop();

      // Do NOT stop audio if the spawn chime is playing
      if (!g_audioOneShot && !g_chimeActive) {
        stopAudio();
      }

      stopFullBlink();
      stopEmptyBlink();
      fillRing(RED);
    }
  }

  // Audio keep-alive
  if (playing) handleAudio();

  // In normal (looping) mode, stop audio if no active hold
  if (!g_audioOneShot && !holdActive && playing) {
    stopAudio();
  }

  tickFullBlink();
  tickYellowBlink();
  tickEmptyBlink();
  tickBonusRainbow();
  tickScheduledAudio();   // run any deferred audio stop

  // R4.5 animation & detection
  if (r45Active) {
    uint32_t now = millis();

    // Move green (unless we've used and failed)
    if (!(r45Used && !r45Success) && (int32_t)(now - r45NextStepAt) >= 0) {
      r45NextStepAt = now + r45StepMs;
      int16_t np = (int16_t)r45Pos + r45Dir;
      if (np < 0) { np = 1; r45Dir = +1; }
      if (np >= (int16_t)GAUGE_LEN) { np = GAUGE_LEN-2; r45Dir = -1; }
      r45Pos = (uint8_t)np;
      drawR45Frame();
    }

    // Attempt logic: arrival based
    if (!r45Used && tagPresent && !r45Attempting) {
      // Start attempt immediately on first arrival
      r45Attempting = true;
      r45AttemptStart = now;
      // If not inside segment at arrival, immediate miss
      if (!r45Inside(r45Pos)) {
        r45Used = true; r45Success = false; r45FreezePos = r45Pos;
        // freeze & flash red on ring
        fillRing(RED);
        drawR45Frame();
        sendRound45Result(false, r45FreezePos, r45SegStart, r45SegLen);
      }
    }
    if (!r45Used && r45Attempting) {
      if (!tagPresent) {
        // Tag removed during attempt → treat as miss unless already succeeded
        if (!r45Success) {
          r45Used = true; r45Success = false; r45FreezePos = r45Pos;
          fillRing(RED);
          drawR45Frame();
          sendRound45Result(false, r45FreezePos, r45SegStart, r45SegLen);
        }
      } else {
        if (r45Inside(r45Pos)) {
          if ((now - r45AttemptStart) >= R45_HOLD_MS) {
            // Success
            r45Used = true; r45Success = true;
            playBonusSpawnChime();        // success sound
            // optional: flash segment once (drawR45Frame already shows it)
            sendRound45Result(true, r45Pos, r45SegStart, r45SegLen);
          }
        } else {
          // Moved outside before meeting hold → miss
          r45Used = true; r45Success = false; r45FreezePos = r45Pos;
          fillRing(RED);
          drawR45Frame();
          sendRound45Result(false, r45FreezePos, r45SegStart, r45SegLen);
        }
      }
    }
  }

}
