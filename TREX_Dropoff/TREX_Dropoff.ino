/*
  TREX – Drop-off Station (Feather S3 + 4×RC522 + 4×Rings + 2×Gauges + MAX98357)
  Tap-to-drop; play clip once ONLY when team score increases on DROP_RESULT.

  • RC522 (x4)   CS: {5,14,18,17}  RST: 11   SPI: SCK=36 MISO=37 MOSI=35
  • 14-px rings  GPIO: {33, 38, 1, 3}
  • 62-px gauges GPIO: {7, 10}
  • I²S audio    BCLK 43 | LRCLK 44 | DOUT 12
  • Transport    ESP-NOW via TrexTransport (channel must match T-Rex)
*/

// ======= AUDIO BACKEND SELECTOR =======
// 1 = PROGMEM (embed .wav in code)   |  0 = LittleFS (+ buffer)
#define TREX_AUDIO_PROGMEM 0
// =====================================

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_NeoPixel.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#include <cstring>

#if TREX_AUDIO_PROGMEM
  #include <pgmspace.h>
  #include <AudioFileSourcePROGMEM.h>
  #include "LootDrop.h"  // const unsigned char LootDrop_wav[] PROGMEM; const unsigned int LootDrop_wav_len;
#else
  #include <LittleFS.h>
  #include <AudioFileSourceLittleFS.h>
  #include <AudioFileSourceBuffer.h>
  constexpr char CLIP_PATH[] = "/LootDrop.wav";
#endif

#include <TrexProtocol.h>
#include <TrexTransport.h>
#include <Preferences.h>
#include "TrexMaintenance.h"

// ---------- Wi-Fi for maintenance ----------
#define WIFI_SSID   "AndrewiPhone"
#define WIFI_PASS   "12345678"
#define HOSTNAME    "Drop-off"    // unique per device

/* ── IDs & radio ───────────────────────────────────────────── */
constexpr uint8_t  STATION_ID    = 6;    // drop-off station id
static uint8_t     WIFI_CHANNEL  = 6;    // must match T-Rex (loaded from NVS)

static bool TX_FRAMED        = false;  // false = legacy packets (no wire header)
static bool RX_ACCEPT_LEGACY = true;   // true = accept packets without wire header

static volatile bool   gRadioCfgPending = false;
static RadioCfgPayload gRadioCfgMsg{};

static void loadRadioConfig() {
  Preferences p;
  p.begin("trex", true);
  uint8_t ch  = p.getUChar("chan", WIFI_CHANNEL);
  uint8_t txf = p.getUChar("txf",  0);
  uint8_t rxl = p.getUChar("rxl",  1);
  p.end();

  if (ch < 1 || ch > 13) ch = WIFI_CHANNEL;
  WIFI_CHANNEL     = ch;
  TX_FRAMED        = (txf != 0);
  RX_ACCEPT_LEGACY = (rxl != 0);

  Serial.printf("[RADIO] Loaded: chan=%u txFramed=%u rxLegacy=%u",
                (unsigned)WIFI_CHANNEL,
                (unsigned)(TX_FRAMED ? 1 : 0),
                (unsigned)(RX_ACCEPT_LEGACY ? 1 : 0));
}

static void saveRadioConfig(uint8_t ch, bool txFramed, bool rxLegacy) {
  Preferences p;
  p.begin("trex", false);
  p.putUChar("chan", ch);
  p.putUChar("txf",  txFramed ? 1 : 0);
  p.putUChar("rxl",  rxLegacy ? 1 : 0);
  p.end();
}

static void applyRadioCfgAndReboot(const RadioCfgPayload& msg) {
  uint8_t ch = msg.wifiChannel;
  bool txf = (msg.txFramed != 0);
  bool rxl = (msg.rxLegacy != 0);

  if (ch < 1 || ch > 13) ch = WIFI_CHANNEL;

  Serial.printf("[RADIO] Apply: chan=%u txFramed=%u rxLegacy=%u (rebooting)",
                (unsigned)ch,
                (unsigned)(txf ? 1 : 0),
                (unsigned)(rxl ? 1 : 0));

  saveRadioConfig(ch, txf, rxl);
  delay(150);
  ESP.restart();
}


/* ── LEDs / gauges ────────────────────────────────────────── */
constexpr uint16_t GAUGE_LEN        = 61;
constexpr uint8_t  RING_BRIGHTNESS  = 64;
constexpr uint8_t  GAUGE_BRIGHTNESS = 255;

/* Map teamScore → LEDs; set TEAM_GOAL to “full bar” score */
constexpr uint32_t TEAM_GOAL = 100;  // 1:1 mapping by default

// --- One-at-a-time scan lock (unlocks on DROP_RESULT or timeout) ---
static bool     scanLocked = false;
static uint32_t scanUnlockAt = 0;
constexpr uint32_t SCAN_LOCK_TIMEOUT_MS = 1000;  // safety if result is delayed

// --- Short exclusive audio window (force-stop after 500ms) ---
constexpr uint32_t AUDIO_EXCLUSIVE_MS = 750;
static uint32_t    audioEndAt = 0;

// (Keep your round-robin index if you have it; it helps fairness)
static uint8_t rrStart = 0;

/* ── pins ─────────────────────────────────────────────────── */
constexpr uint8_t PIN_SCK   = 36, PIN_MISO = 37, PIN_MOSI = 35, PIN_RST = 11;
constexpr uint8_t CS_PINS[4]   = { 5, 14, 18, 17 };
constexpr uint8_t RING_PINS[4] = { 33, 38,  1,  3 };
constexpr uint8_t GAUGE_PINS[2]= {  7, 10 };

constexpr int PIN_I2S_BCLK  = 43;
constexpr int PIN_I2S_LRCLK = 44;
constexpr int PIN_I2S_DOUT  = 12;

/* ── objects ───────────────────────────────────────────────── */
MFRC522 rfid[4] = {
  { CS_PINS[0], PIN_RST }, { CS_PINS[1], PIN_RST },
  { CS_PINS[2], PIN_RST }, { CS_PINS[3], PIN_RST }
};
Adafruit_NeoPixel ring[4] = {
  Adafruit_NeoPixel(14, RING_PINS[0], NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(14, RING_PINS[1], NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(14, RING_PINS[2], NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(14, RING_PINS[3], NEO_GRB + NEO_KHZ800)
};
Adafruit_NeoPixel gauge[2] = {
  Adafruit_NeoPixel(GAUGE_LEN, GAUGE_PINS[0], NEO_GRB + NEO_KHZ800),
  Adafruit_NeoPixel(GAUGE_LEN, GAUGE_PINS[1], NEO_GRB + NEO_KHZ800)
};

/* audio (conditional) */
#if TREX_AUDIO_PROGMEM
  AudioFileSourcePROGMEM *wavSrc = nullptr;
#else
  AudioFileSourceLittleFS *wavFile = nullptr;
  AudioFileSourceBuffer   *wavBuf  = nullptr;
#endif
AudioGeneratorWAV *decoder = nullptr;
AudioOutputI2S    *i2sOut  = nullptr;
bool               playing = false;          // one-shot playback state
bool               audioExclusive = false;   // freeze LEDs/RFID during playback

/* colours */
static inline uint32_t C(uint8_t r,uint8_t g,uint8_t b){ return Adafruit_NeoPixel::Color(r,g,b); }
const uint32_t RED   = C(255,0,0);
const uint32_t GREEN = C(0,255,0);
const uint32_t GOLD  = C(255, 180, 0);
const uint32_t WHITE = C(255,255,255);
const uint32_t OFF   = 0;

/* tag tracking — edge-trigger (tap) */
constexpr uint32_t ABSENCE_MS  = 150;
bool     tagPresent[4] = {0};
uint32_t absentMs[4]   = {0};

/* game/broadcast state */
volatile bool       gameActive   = true;    // onRx flips this
bool                wasPaused    = false;
volatile LightState g_lightState = LightState::GREEN;
volatile uint32_t   teamScore    = 0;
static bool     gaugeDirty = false;         // a repaint is waiting
static uint32_t pendingTeamScore = 0;       // latest score to render

// Current round mapping info (from ROUND_STATUS)
volatile uint8_t    roundIndex        = 1;
volatile uint32_t   roundStartScore   = 0;
volatile uint32_t   roundGoalAbs      = 100;
inline uint32_t roundTargetCount() {
  return (roundGoalAbs > roundStartScore) ? (roundGoalAbs - roundStartScore) : 100;
}

// --- Ring "hold-green" for 1s after a successful DROP_RESULT ---
static uint32_t ringHoldUntil[4] = {0,0,0,0};
static bool     ringHoldActive[4] = {false,false,false,false};
static bool ringPendingShow[4] = {false,false,false,false};

// If you don’t have it yet and want a safe mapping from results to readers:
static int8_t   reqQueue[4];  // small FIFO of pending reader indexes
static uint8_t  reqHead = 0, reqTail = 0;

inline bool reqEnqueue(int8_t idx) { uint8_t n=(reqTail+1)&3; if(n==reqHead) return false; reqQueue[reqTail]=idx; reqTail=n; return true; }
inline int8_t reqDequeue(){ if(reqHead==reqTail) return -1; int8_t v=reqQueue[reqHead]; reqHead=(reqHead+1)&3; return v; }

// --- Post-game final bars blink (red blinks, green solid) ---
static bool     finalBlinkActive  = false;
static bool     finalBlinkOn      = false;
static uint32_t finalBlinkLastMs  = 0;
constexpr uint32_t FINAL_BLINK_PERIOD_MS = 500;
static uint32_t finalScoreSnapshot = 0;   // score to display during the blink

/* misc */
uint16_t g_seq = 1;

// --- Maintenance flag for network-triggered entry ---
static bool maintRequested = false;
static bool maintLEDOn     = false;

/* ── WAV header helpers ───────────────────────────────────── */
static inline uint16_t rd16(const uint8_t* p){ return (uint16_t)p[0] | ((uint16_t)p[1]<<8); }
static inline uint32_t rd32(const uint8_t* p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static bool parseWavHeader(const uint8_t* buf, size_t n, uint32_t& rate, uint16_t& bits, uint16_t& ch, uint16_t& fmt) {
  if (n < 44) return false;
  if (memcmp(buf+0, "RIFF",4)!=0 || memcmp(buf+8, "WAVE",4)!=0) return false;
  size_t i=12;
  while (i+8<=n) {
    const uint8_t* ck = buf+i; uint32_t cksz=rd32(ck+4);
    if (memcmp(ck,"fmt ",4)==0) {
      if (i+8+16>n) return false;
      const uint8_t* f = ck+8;
      fmt=rd16(f+0); ch=rd16(f+2); rate=rd32(f+4); bits=rd16(f+14);
      return true;
    }
    i += 8 + cksz;
  }
  return false;
}
#if TREX_AUDIO_PROGMEM
static bool detectProgmemWav(uint32_t& rate, uint16_t& bits, uint16_t& ch, uint16_t& fmt,
                             const uint8_t* p, size_t len) {
  uint8_t tmp[128]; size_t n = (len<sizeof(tmp))?len:sizeof(tmp); memcpy_P(tmp,p,n);
  return parseWavHeader(tmp,n,rate,bits,ch,fmt);
}
#else
static bool detectFsWav(uint32_t& rate, uint16_t& bits, uint16_t& ch, uint16_t& fmt,
                        const char* path) {
  File f=LittleFS.open(path,"r"); if(!f) return false;
  uint8_t tmp[128]; size_t n=f.read(tmp,sizeof(tmp)); f.close();
  return parseWavHeader(tmp,n,rate,bits,ch,fmt);
}
#endif

/* ── audio helpers ────────────────────────────────────────── */
static bool openChain() {
  if (decoder && decoder->isRunning()) decoder->stop();

#if TREX_AUDIO_PROGMEM
  if (wavSrc) { delete wavSrc; wavSrc=nullptr; }
  wavSrc = new AudioFileSourcePROGMEM(LootDrop_wav, LootDrop_wav_len);
#else
  if (wavBuf)  { delete wavBuf;  wavBuf=nullptr; }
  if (wavFile) { delete wavFile; wavFile=nullptr; }
  wavFile = new AudioFileSourceLittleFS(CLIP_PATH);
  if (!wavFile) { Serial.println("[DROP] wavFile alloc fail"); return false; }
  wavBuf = new AudioFileSourceBuffer(wavFile, 8192);  // 8 KB headroom
  if (!wavBuf) { Serial.println("[DROP] wavBuf alloc fail"); return false; }
#endif

  if (!decoder) decoder = new AudioGeneratorWAV();
  bool ok =
  #if TREX_AUDIO_PROGMEM
    decoder->begin(wavSrc, i2sOut);
  #else
    decoder->begin(wavBuf, i2sOut);
  #endif
  if (!ok) { Serial.println("[DROP] decoder.begin() failed"); return false; }

  // force I²S rate to the WAV header every time
  uint32_t rate=0; uint16_t bits=0, ch=0, fmt=0;
#if TREX_AUDIO_PROGMEM
  if (detectProgmemWav(rate,bits,ch,fmt, LootDrop_wav, LootDrop_wav_len) && rate) {
    i2sOut->SetRate(rate);
    Serial.printf("[DROP] WAV fmt=%u ch=%u bits=%u rate=%u\n", fmt, ch, bits, rate);
  }
#else
  if (detectFsWav(rate,bits,ch,fmt, CLIP_PATH) && rate) {
    i2sOut->SetRate(rate);
    Serial.printf("[DROP] WAV fmt=%u ch=%u bits=%u rate=%u\n", fmt, ch, bits, rate);
  }
#endif
  return true;
}

inline void pumpAudio() { if (playing && decoder) decoder->loop(); }

// one-shot playback (no auto-restart)
static void startAudioExclusiveShort() {
  audioExclusive = true;
  audioEndAt = millis() + AUDIO_EXCLUSIVE_MS;
  if (playing) { decoder->stop(); playing=false; }
  playing = openChain();
}

static void stopAudioExclusive() {
  if (playing) { decoder->stop(); playing=false; }
  audioExclusive = false;

  // Flush any ring shows that were suppressed during audio
  for (uint8_t i=0; i<4; ++i) {
    if (ringPendingShow[i]) {
      ring[i].show();
      ringPendingShow[i] = false;
    }
  }
  if (gaugeDirty) { drawTeamGaugesRound(teamScore, roundTargetCount()); gaugeDirty = false; }
}

void maintainRingHolds() {
  const uint32_t now = millis();
  for (uint8_t i=0; i<4; ++i) {
    if (ringHoldActive[i] && (int32_t)(now - ringHoldUntil[i]) >= 0) {
      ringHoldActive[i] = false;
      fillRing(i, RED);    // back to ready
    }
  }
}

/* ── helpers ─────────────────────────────────────────────── */
bool cardPresent(MFRC522 &m) {
  byte atqa[2]; byte len = 2;
  return m.PICC_WakeupA(atqa, &len) == MFRC522::STATUS_OK;
}
bool readUid(MFRC522 &m, TrexUid &out) {
  if (!m.PICC_ReadCardSerial()) return false;
  out.len = m.uid.size;
  for (uint8_t i=0;i<out.len && i<10;i++) out.bytes[i] = m.uid.uidByte[i];
  return true;
}
void packHeader(uint8_t type, uint16_t payLen, uint8_t* buf) {
  auto* h = (MsgHeader*)buf;
  h->version      = TREX_PROTO_VERSION;
  h->type         = type;
  h->srcStationId = STATION_ID;
  h->flags        = 0;
  h->payloadLen   = payLen;
  h->seq          = g_seq++;
}

void fillRing(uint8_t idx, uint32_t c) {
  for (uint16_t p = 0; p < ring[idx].numPixels(); ++p) {
    ring[idx].setPixelColor(p, c);
    if ((p & 7) == 0) pumpAudio();
  }
  if (!audioExclusive) {
    ring[idx].show();
  } else {
    ringPendingShow[idx] = true;        // <-- will flush after audio ends
  }
  pumpAudio();
}

void drawTeamGaugesRound(uint32_t score, uint32_t target) {
  // progress this round
  uint32_t prog = (score > roundStartScore) ? (score - roundStartScore) : 0;
  if (target == 0) target = 1; // avoid div0

  // Map % to the full pipe, reserve top green marker (in-game)
  const bool goalReached = (prog >= target);
  uint16_t lit = 0;
  if (!goalReached) {
    // generous ceil-ish mapping below the top marker
    uint32_t scaled = prog * (uint32_t)GAUGE_LEN;
    lit = (uint16_t)(scaled / target);
  } else {
    lit = GAUGE_LEN; // full bar
  }
  const uint16_t top = (GAUGE_LEN > 0) ? (GAUGE_LEN - 1) : 0;
  const uint16_t goldCount = goalReached ? GAUGE_LEN : (uint16_t)min<uint16_t>(lit, top);

  for (auto &g : gauge) {
    // In-game paint: GOLD progress, remainder OFF
    // (If goal reached, we paint all GREEN below)
    for (uint16_t i = 0; i < GAUGE_LEN; ++i) {
      uint32_t c;
      if (goalReached) {
        c = GREEN;                       // goal: whole bar GREEN
      } else {
        c = (i < goldCount) ? GOLD : OFF;
      }
      g.setPixelColor(i, c);
      if ((i & 15) == 0) pumpAudio();
    }

    // Deterministic top marker: always green during the game
    if (!goalReached && GAUGE_LEN > 0) {
      g.setPixelColor(top, GREEN);
    }

    if (!audioExclusive) g.show();
    pumpAudio();
  }
}

void drawFinalBarsFrame(uint32_t score, uint32_t target, bool redOn) {
  // Map score to the full gauge length (no reserved marker in post-game)
  uint16_t lit = 0;
  if (target == 0) target = 1;
  const uint32_t clamped = (score > target) ? target : score;
  const uint32_t scaled  = clamped * GAUGE_LEN + (target - 1);
  lit = (uint16_t)min<uint32_t>(GAUGE_LEN, scaled / TEAM_GOAL);

  for (auto &g : gauge) {
    for (uint16_t i=0; i<GAUGE_LEN; ++i) {
      const bool inGreen = (i < lit);
      const uint32_t c = inGreen ? GREEN : (redOn ? RED : OFF);
      g.setPixelColor(i, c);
      if ((i & 15) == 0) pumpAudio();
    }
    if (!audioExclusive) g.show();
    pumpAudio();
  }
}

void startFinalBlink(uint32_t score, uint32_t target) {
  finalScoreSnapshot = score;
  finalBlinkActive   = true;
  finalBlinkOn       = true;
  finalBlinkLastMs   = millis();
  drawFinalBarsFrame(finalScoreSnapshot, target, finalBlinkOn);
}

void stopFinalBlink() {
  finalBlinkActive = false;
}

void tickFinalBlink() {
  if (!finalBlinkActive) return;
  const uint32_t now = millis();
  if ((now - finalBlinkLastMs) >= FINAL_BLINK_PERIOD_MS) {
    finalBlinkLastMs = now;
    finalBlinkOn = !finalBlinkOn;
    drawFinalBarsFrame(finalScoreSnapshot, roundTargetCount(), finalBlinkOn);
  }
}

/* ── NET: messages ───────────────────────────────────────── */
void sendHello() {
  uint8_t buf[sizeof(MsgHeader)+sizeof(HelloPayload)];
  packHeader((uint8_t)MsgType::HELLO, sizeof(HelloPayload), buf);
  auto* p = (HelloPayload*)(buf + sizeof(MsgHeader));
  p->stationType = (uint8_t)StationType::DROP;
  p->stationId   = STATION_ID;
  p->fwMajor = 0; p->fwMinor = 3;
  p->wifiChannel = WIFI_CHANNEL;
  memset(p->mac, 0, 6);
  Transport::sendToServer(buf, sizeof(buf));
}
void sendDropRequest(const TrexUid& uid, uint8_t readerIndex) {
  uint8_t buf[sizeof(MsgHeader)+sizeof(DropRequestPayload)];
  packHeader((uint8_t)MsgType::DROP_REQUEST, sizeof(DropRequestPayload), buf);
  auto* p = (DropRequestPayload*)(buf + sizeof(MsgHeader));
  p->uid = uid; p->readerIndex = readerIndex;
  Transport::sendToServer(buf, sizeof(buf));
}

/* ── RX handler ──────────────────────────────────────────── */
void onRx(const uint8_t* data, uint16_t len) {
  if (len < sizeof(MsgHeader)) return;
  auto* h = (const MsgHeader*)data;
  if (h->version != TREX_PROTO_VERSION) {
    Serial.printf("[WARN] Proto mismatch on RX: got=%u exp=%u (type=%u)\n",
                  h->version, (unsigned)TREX_PROTO_VERSION, h->type);
    return;
  }

  // RADIO_CFG from server: persist new radio settings and reboot
  if ((MsgType)h->type == MsgType::RADIO_CFG) {
    if (h->payloadLen == sizeof(RadioCfgPayload) && h->srcStationId == 0) {
      const auto* p = (const RadioCfgPayload*)(data + sizeof(MsgHeader));
      gRadioCfgMsg = *p;
      gRadioCfgPending = true;
      Serial.printf("[RADIO] RADIO_CFG received: chan=%u txFramed=%u rxLegacy=%u\n",
                    (unsigned)p->wifiChannel,
                    (unsigned)p->txFramed,
                    (unsigned)p->rxLegacy);
    }
    return;
  }

  switch ((MsgType)h->type) {
    case MsgType::CONTROL_CMD: {
      if (h->payloadLen != sizeof(ControlCmdPayload)) break;
      auto* p = (const ControlCmdPayload*)(data + sizeof(MsgHeader));

      const uint8_t myType = (uint8_t)StationType::DROP;
      const uint8_t myId   = STATION_ID;  // 6 for your Drop-off

      bool typeMatch = (p->targetType == myType || p->targetType == 255);
      bool idMatch   = (p->targetId   == myId   || p->targetId   == 255);
      bool matches   = typeMatch && idMatch;

      if (!matches) break;

      if ((ControlOp)p->op == ControlOp::ENTER_MAINT) {
        maintRequested = true;
        Serial.println("[DROP] CONTROL_CMD ENTER_MAINT (targeted) received");
      }
      break;
    }

    case MsgType::ROUND_STATUS: {
      if (h->payloadLen != sizeof(RoundStatusPayload)) break;
      auto* p = (const RoundStatusPayload*)(data + sizeof(MsgHeader));
      roundIndex      = p->roundIndex;
      roundStartScore = p->roundStartScore;
      roundGoalAbs    = p->roundGoalAbs;

      // Repaint immediately (unless audioExclusive)
      if (gameActive) {
        if (!audioExclusive) drawTeamGaugesRound(teamScore, roundTargetCount());
        else { pendingTeamScore = teamScore; gaugeDirty = true; }
      }
      break;
    }

    case MsgType::STATE_TICK: {
      if (h->payloadLen != sizeof(StateTickPayload)) break;
      auto* p = (const StateTickPayload*)(data + sizeof(MsgHeader));
      if      (p->state == (uint8_t)LightState::GREEN)  g_lightState = LightState::GREEN;
      else if (p->state == (uint8_t)LightState::YELLOW) g_lightState = LightState::YELLOW;
      else                                              g_lightState = LightState::RED;
      break;
    }
    
    case MsgType::DROP_RESULT: {
      if (h->payloadLen < 6) break;

      const uint8_t* pl = data + sizeof(MsgHeader);
      uint32_t newTeamScore =  (uint32_t)pl[2]
                            | ((uint32_t)pl[3] << 8)
                            | ((uint32_t)pl[4] << 16)
                            | ((uint32_t)pl[5] << 24);

      uint32_t prev = teamScore;
      teamScore = newTeamScore;

      int8_t idxFromFifo = reqDequeue();

      int8_t idx = -1;
      if (h->payloadLen >= 7) {
        uint8_t readerIndex = pl[6];
        if (readerIndex < 4) idx = (int8_t)readerIndex;
      }
      if (idx < 0) idx = idxFromFifo;

      if (!audioExclusive) {
        drawTeamGaugesRound(teamScore, roundTargetCount());
      } else {
        pendingTeamScore = teamScore;
        gaugeDirty = true;
      }

      if (teamScore > prev) {
        if (idx >= 0 && idx < 4) {
          for (uint8_t j = 0; j < 4; ++j) {
            if (j != idx && ringHoldActive[j]) {
              ringHoldActive[j] = false;
              if (!tagPresent[j] && !audioExclusive) fillRing(j, RED);
            }
          }

          ringHoldActive[idx] = true;
          ringHoldUntil[idx]  = millis() + 1000;
          fillRing((uint8_t)idx, GREEN);

          scanLocked   = true;
          scanUnlockAt = ringHoldUntil[idx];
        }
        startAudioExclusiveShort();

      } else {
        if (idx >= 0 && idx < 4 && !ringHoldActive[idx] && !audioExclusive) {
          if (!tagPresent[idx]) fillRing((uint8_t)idx, RED);
        }
        scanLocked = false;
      }
      break;
    }

    case MsgType::SCORE_UPDATE: {
      if (h->payloadLen != sizeof(ScoreUpdatePayload)) break;
      auto* p = (const ScoreUpdatePayload*)(data + sizeof(MsgHeader));
      teamScore = p->teamScore;
      if (gameActive) {
        if (!audioExclusive) drawTeamGaugesRound(teamScore, roundTargetCount());
        else { pendingTeamScore = teamScore; gaugeDirty = true; }
      } else {
        finalScoreSnapshot = teamScore;
      }
      break;
    }

    case MsgType::GAME_START: {
      gameActive = true;
      Serial.println("[DROP] GAME_START");
      stopFinalBlink();
      scanLocked = false;
      stopAudioExclusive();

      for (int i=0;i<4;i++) {
        ringHoldActive[i] = false;
        ringPendingShow[i] = false;
        tagPresent[i] = false;
        absentMs[i]   = 0;
        fillRing(i, RED);
      }
      reqHead = reqTail = 0;

      drawTeamGaugesRound(teamScore, roundTargetCount());
      break;
    }

    case MsgType::GAME_OVER: {
      gameActive = false;
      Serial.printf("[DROP] GAME_OVER  final score=%lu / %lu\n",
                    (unsigned long)teamScore, (unsigned long)TEAM_GOAL);

      stopAudioExclusive();

      for (int i=0;i<4;i++) { tagPresent[i]=false; absentMs[i]=0; fillRing(i, RED); }

      startFinalBlink(teamScore - roundStartScore, roundTargetCount());
      break;
    }

    default: break;
  }
}

/* ── setup ───────────────────────────────────────────────── */
void setup() {
  Serial.begin(115200);
  delay(50);

  loadRadioConfig();
  Serial.println("\n[DROP] Boot");

#if !TREX_AUDIO_PROGMEM
  LittleFS.begin();
  File f = LittleFS.open(CLIP_PATH, "r");
  if (!f) Serial.println("[DROP] Missing file on LittleFS");
  else { Serial.printf("[DROP] WAV size: %u bytes\n", (unsigned)f.size()); f.close(); }
#endif

  i2sOut = new AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S);
  i2sOut->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT);
  i2sOut->SetGain(1.0f);
  i2sOut->SetRate(48000);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);
  for (auto &r : rfid) r.PCD_Init();

  for (uint8_t i=0; i<4; ++i) { ring[i].begin(); ring[i].setBrightness(RING_BRIGHTNESS); }
  for (auto &g : gauge)       { g.begin(); g.setBrightness(GAUGE_BRIGHTNESS); g.show(); }
  drawTeamGaugesRound(teamScore, roundTargetCount());
  for (uint8_t i=0; i<4; ++i) fillRing(i, RED);

  TransportConfig cfg{ /*maintenanceMode=*/false, /*wifiChannel=*/WIFI_CHANNEL };
  cfg.txFramed = TX_FRAMED;
  cfg.rxAcceptLegacy = RX_ACCEPT_LEGACY;
  if (!Transport::init(cfg, onRx)) {
    Serial.println("[DROP] Transport init FAILED");
    while (1) delay(1000);
  }
  Serial.printf("Trex proto ver: %d\n", TREX_PROTO_VERSION);
}

/* ── loop ────────────────────────────────────────────────── */
void loop() {
  if (gRadioCfgPending) {
    gRadioCfgPending = false;
    applyRadioCfgAndReboot(gRadioCfgMsg);
    return;
  }

  static Maint::Config mcfg{WIFI_SSID, WIFI_PASS, HOSTNAME,
                            /*apFallback=*/true, /*apChannel=*/WIFI_CHANNEL,
                            /*apPass=*/"trexsetup", /*buttonPin=*/0, /*holdMs=*/1500};
  mcfg.stationType  = StationType::DROP;
  mcfg.stationId    = STATION_ID;
  mcfg.enableBeacon = true;

  // BOOT long-press OR network MAINT request
  bool justEntered = Maint::checkRuntimeEntry(mcfg);
  if (!justEntered && maintRequested) {
    Maint::begin(mcfg);
    justEntered    = true;
    maintRequested = false;
  }
  if (justEntered || Maint::active) {
    if (!maintLEDOn) {
      const uint32_t BLUE = Adafruit_NeoPixel::Color(0,0,255);
      for (uint8_t i=0;i<4;i++) fillRing(i, BLUE);
      maintLEDOn = true;
      Serial.println("[DROP] Maintenance mode entered");
    }
    Maint::loop();
    return;
  } else if (maintLEDOn) {
    // If we ever exit maintenance without reboot, restore base visuals
    maintLEDOn = false;
    for (uint8_t i=0;i<4;i++) fillRing(i, RED);
    drawTeamGaugesRound(teamScore, roundTargetCount());
    Serial.println("[DROP] Maintenance mode exited");
  }

  Transport::loop();

  if (audioExclusive) {
    if (playing && decoder) {
      if (!decoder->loop()) { stopAudioExclusive(); return; }
    }
    if ((int32_t)(millis() - audioEndAt) >= 0) { stopAudioExclusive(); return; }
    return;
  }

  if (!gameActive) {
    if (!wasPaused) {
      wasPaused = true;
      for (int i=0;i<4;i++) { tagPresent[i]=false; absentMs[i]=0; fillRing(i, RED); }
      stopAudioExclusive();
    }
    tickFinalBlink();
    maintainRingHolds();
    return;
  }

  const uint32_t now = millis();

  if (scanLocked && (int32_t)(now - scanUnlockAt) >= 0) {
    scanLocked = false;
    int8_t dropped = reqDequeue();
    if (dropped >= 0 && !ringHoldActive[dropped] && !audioExclusive) {
      if (!tagPresent[dropped]) fillRing((uint8_t)dropped, RED);
    }
  }

  if (!scanLocked) {
    for (uint8_t step = 0; step < 4; ++step) {
      const uint8_t i = (rrStart + step) & 3;
      MFRC522 &rd = rfid[i];

      bool present = cardPresent(rd);

      if (tagPresent[i]) {
        if (!present) {
          if (absentMs[i] == 0) absentMs[i] = now;
          else if (now - absentMs[i] >= ABSENCE_MS) {
            tagPresent[i] = false; absentMs[i] = 0;
            if (!ringHoldActive[i]) fillRing(i, RED);
          }
        } else {
          absentMs[i] = 0;
        }
        continue;
      }

      if (!present) continue;

      TrexUid uid;
      if (readUid(rd, uid)) {
        sendDropRequest(uid, i);

        fillRing(i, WHITE);
        tagPresent[i] = true;
        reqEnqueue(i);

        scanLocked   = true;
        scanUnlockAt = now + SCAN_LOCK_TIMEOUT_MS;
        break;
      }

      pumpAudio();
    }

    rrStart = (rrStart + 1) & 3;
  }

  if (gameActive) {
    if (audioExclusive) {
      pendingTeamScore = teamScore;
      gaugeDirty = true;
    } else {
      drawTeamGaugesRound(teamScore, roundTargetCount());
    }
  } else {
    tickFinalBlink();
  }

  maintainRingHolds();
}
