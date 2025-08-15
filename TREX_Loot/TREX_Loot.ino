/*
  TREX – Loot Station (Feather S3 + RC522 + MAX98357 + Ring + Gauge)
  ------------------------------------------------------------------
  • RC522              CS=5  RST=6   SPI: SCK=36 MISO=37 MOSI=35
  • 14-px ring         GPIO 38
  • 56-px pipe gauge   GPIO 18   (set GAUGE_LEN to your strip length)
  • MOSFET light       GPIO 17
  • I²S audio          BCLK 43 | LRCLK 44 | DOUT 33
  • Transport          ESP-NOW via TrexTransport (channel must match T-Rex)
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

#if TREX_AUDIO_PROGMEM
  #include <AudioFileSourcePROGMEM.h>
  #include "replenish.h"  // const … PROGMEM array + length
#else
  #include <LittleFS.h>
  #include <AudioFileSourceLittleFS.h>
  #include <AudioFileSourceBuffer.h>
  constexpr char CLIP_PATH[] = "/replenish.wav";
#endif

#include <TrexProtocol.h>
#include <TrexTransport.h>

/* ── IDs & radio ───────────────────────────────────────────── */
constexpr uint8_t  STATION_ID    = 1;    // ← set unique 1..5 for each loot station
constexpr uint8_t  WIFI_CHANNEL  = 6;    // ← must match the T-Rex server

/* ── LED config ────────────────────────────────────────────── */
constexpr uint16_t GAUGE_LEN        = 56;     // pipe gauge length (px)
constexpr uint8_t  RING_BRIGHTNESS  = 64;
constexpr uint8_t  GAUGE_BRIGHTNESS = 255;

/* ── pins ──────────────────────────────────────────────────── */
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

/* ── objects ───────────────────────────────────────────────── */
MFRC522 rfid(PIN_RFID_CS, PIN_RFID_RST);
Adafruit_NeoPixel ring (14,         PIN_RING,  NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel gauge(GAUGE_LEN,  PIN_GAUGE, NEO_GRB + NEO_KHZ800);

/* audio (conditional) */
#if TREX_AUDIO_PROGMEM
  AudioFileSourcePROGMEM *wavSrc = nullptr;
#else
  AudioFileSourceLittleFS *wavFile = nullptr;
  AudioFileSourceBuffer   *wavBuf  = nullptr;
#endif
AudioGeneratorWAV *decoder = nullptr;
AudioOutputI2S    *i2sOut  = nullptr;
bool               playing = false;

/* ── colours ──────────────────────────────────────────────── */
static inline uint32_t C_RGB(uint8_t r,uint8_t g,uint8_t b){ return Adafruit_NeoPixel::Color(r,g,b); }
const uint32_t RED     = C_RGB(255,0,0);
const uint32_t GREEN   = C_RGB(0,255,0);
const uint32_t WHITE   = C_RGB(255,255,255);
const uint32_t OFF     = 0;

/* ── RFID presence debounce ───────────────────────────────── */
constexpr uint32_t ABSENCE_MS = 150;   // debounce removal
bool          tagPresent      = false;
uint32_t      absentStartMs   = 0;

/* ── Game/hold state (server-auth) ────────────────────────── */
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

/* ── misc ─────────────────────────────────────────────────── */
uint16_t g_seq = 1;            // outgoing message seq
uint32_t lastHelloMs = 0;

/* ── audio helpers ────────────────────────────────────────── */
static bool openChain() {
  if (decoder && decoder->isRunning()) decoder->stop();

#if TREX_AUDIO_PROGMEM
  if (wavSrc) { delete wavSrc; wavSrc = nullptr; }
  wavSrc = new AudioFileSourcePROGMEM(replenish_wav, replenish_wav_len);
#else
  if (wavBuf)  { delete wavBuf;  wavBuf  = nullptr; }
  if (wavFile) { delete wavFile; wavFile = nullptr; }
  wavFile = new AudioFileSourceLittleFS(CLIP_PATH);
  if (!wavFile) { Serial.println("[LOOT] wavFile alloc fail"); return false; }
  // 4 KB buffer smooths over LED .show() stalls
  wavBuf = new AudioFileSourceBuffer(wavFile, 4096);
  if (!wavBuf) { Serial.println("[LOOT] wavBuf alloc fail"); return false; }
#endif

  if (!decoder) decoder = new AudioGeneratorWAV();
#if TREX_AUDIO_PROGMEM
  bool ok = decoder->begin(wavSrc, i2sOut);
#else
  bool ok = decoder->begin(wavBuf, i2sOut);
#endif
  if (!ok) Serial.println("[LOOT] decoder.begin() failed");
  return ok;
}

bool startAudio() {
  if (playing) return true;
  playing = openChain();
  return playing;
}

void stopAudio() {
  if (!playing) return;
  decoder->stop();        // clean stop so next begin() works
  playing = false;
}

inline void handleAudio() {
  if (!playing || !decoder) return;

  if (!decoder->loop()) {       // EOF or starvation
    decoder->stop();
    playing = openChain();      // reopen and continue
    if (!playing) Serial.println("[LOOT] audio re-begin failed");
  }
}

/* ── helpers ──────────────────────────────────────────────── */
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
  h->srcStationId = STATION_ID;
  h->flags        = 0;
  h->payloadLen   = payLen;
  h->seq          = g_seq++;
}

/* ── LED drawing ──────────────────────────────────────────── */
inline void pumpAudio() { handleAudio(); }  // feed while we block on LED shows

void drawRingCarried(uint8_t cur, uint8_t maxC) {
  pumpAudio();
  const uint16_t n = ring.numPixels();
  uint16_t lit = 0;
  if (maxC > 0) lit = (uint16_t)((uint32_t)cur * n + (maxC-1)) / maxC;  // ceil
  for (uint16_t i=0;i<n;i++) ring.setPixelColor(i, (i<lit) ? GREEN : OFF);
  ring.show();
}

void drawGaugeInventory(uint16_t inventory, uint16_t capacity) {
  pumpAudio();
  uint16_t lit = 0;
  if (capacity > 0) lit = (uint16_t)((uint32_t)inventory * GAUGE_LEN + (capacity-1)) / capacity; // ceil
  for (uint16_t i=0;i<GAUGE_LEN;i++) gauge.setPixelColor(i, (i<lit) ? WHITE : OFF);
  gauge.show();
}

void fillRing(uint32_t c) {
  pumpAudio();
  for (uint16_t i=0;i<ring.numPixels();++i) ring.setPixelColor(i,c);
  ring.show();
}

/* ── NET: messages ────────────────────────────────────────── */
void sendHello() {
  uint8_t buf[sizeof(MsgHeader)+sizeof(HelloPayload)];
  packHeader((uint8_t)MsgType::HELLO, sizeof(HelloPayload), buf);
  auto* p = (HelloPayload*)(buf + sizeof(MsgHeader));
  p->stationType = (uint8_t)StationType::LOOT;
  p->stationId   = STATION_ID;
  p->fwMajor = 0; p->fwMinor = 1;
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
  ((LootHoldStopPayload*)(buf + sizeof(MsgHeader)))->holdId = holdId;
  Transport::sendToServer(buf, sizeof(buf));
  holdActive = false; holdId = 0;
}

/* ── RX handler ───────────────────────────────────────────── */
void onRx(const uint8_t* data, uint16_t len) {
  if (len < sizeof(MsgHeader)) return;
  auto* h = (const MsgHeader*)data;
  if (h->version != TREX_PROTO_VERSION) return;

  switch ((MsgType)h->type) {
    case MsgType::STATE_TICK: {
      if (h->payloadLen != sizeof(StateTickPayload)) break;
      auto* p = (const StateTickPayload*)(data + sizeof(MsgHeader));
      g_lightState = (p->state == (uint8_t)LightState::GREEN) ? LightState::GREEN : LightState::RED;
      if (g_lightState == LightState::RED && !holdActive) fillRing(RED);
      break;
    }

    case MsgType::LOOT_HOLD_ACK: {
      if (h->payloadLen != sizeof(LootHoldAckPayload)) break;
      auto* p = (const LootHoldAckPayload*)(data + sizeof(MsgHeader));
      if (p->holdId != holdId) break;   // ignore stale
      maxCarry = p->maxCarry;
      carried  = p->carried;
      inv      = p->inventory;
      cap      = p->capacity;

      if (p->accepted) {
        holdActive = true;
        startAudio();
        drawRingCarried(carried, maxCarry);
        drawGaugeInventory(inv, cap);
      } else {
        fillRing(RED);
      }
      break;
    }

    case MsgType::LOOT_TICK: {
      if (h->payloadLen != sizeof(LootTickPayload)) break;
      auto* p = (const LootTickPayload*)(data + sizeof(MsgHeader));
      if (!holdActive || p->holdId != holdId) break;
      carried = p->carried;
      inv     = p->inventory;
      drawRingCarried(carried, maxCarry);
      drawGaugeInventory(inv, cap);
      break;
    }

    case MsgType::HOLD_END: {
      if (h->payloadLen != sizeof(HoldEndPayload)) break;
      auto* p = (const HoldEndPayload*)(data + sizeof(MsgHeader));
      if (p->holdId != holdId) break;
      holdActive = false; holdId = 0;
      stopAudio();
      fillRing(RED);
      break;
    }

    case MsgType::STATION_UPDATE: {
      if (h->payloadLen != sizeof(StationUpdatePayload)) break;
      auto* p = (const StationUpdatePayload*)(data + sizeof(MsgHeader));
      if (p->stationId == STATION_ID) {
        inv = p->inventory; cap = p->capacity;
        if (!holdActive && !tagPresent) drawGaugeInventory(inv, cap);
      }
      break;
    }

    case MsgType::GAME_START: {
      gameActive = true;
      Serial.println("[LOOT] GAME_START");
      break;
    }

    case MsgType::GAME_OVER: {
      gameActive = false;
      holdActive = false; holdId = 0;
      carried = 0;
      tagPresent = false; absentStartMs = 0;
      fillRing(RED);
      stopAudio();
      Serial.println("[LOOT] GAME_OVER");
      break;
    }

    default: break;
  }
}

/* ── setup ────────────────────────────────────────────────── */
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n[LOOT] Boot");

  pinMode(PIN_MOSFET, OUTPUT);
  digitalWrite(PIN_MOSFET, HIGH);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);
  rfid.PCD_Init();

  ring.begin();  ring.setBrightness(RING_BRIGHTNESS);  fillRing(RED);
  gauge.begin(); gauge.setBrightness(GAUGE_BRIGHTNESS); gauge.clear(); gauge.show();

  i2sOut = new AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S);
  i2sOut->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT);
  i2sOut->SetGain(1.0f);
  // Do NOT force SetRate(); let WAV header choose the rate

#if !TREX_AUDIO_PROGMEM
  LittleFS.begin();
  // Optional sanity check:
  File f = LittleFS.open(CLIP_PATH, "r");
  if (!f) Serial.println("[LOOT] Missing /replenish.wav on LittleFS");
  else { Serial.printf("[LOOT] WAV size: %u bytes\n", (unsigned)f.size()); f.close(); }
#endif

  TransportConfig cfg{ /*maintenanceMode=*/false, /*wifiChannel=*/WIFI_CHANNEL };
  if (!Transport::init(cfg, onRx)) {
    Serial.println("[LOOT] Transport init FAILED");
    while (1) { delay(1000); }
  }

  Serial.printf("Trex proto ver: %d\n", TREX_PROTO_VERSION);
  drawGaugeInventory(inv, cap);
}

/* ── loop ─────────────────────────────────────────────────── */
void loop() {
  Transport::loop();

  // ---- PAUSED / GAME OVER: only listen for messages ----
  if (!gameActive) {
    if (!wasPaused) {
      wasPaused = true;
      holdActive = false; holdId = 0; carried = 0;
      tagPresent = false; absentStartMs = 0;
      if (playing) stopAudio();
      fillRing(RED);
    }
    static uint32_t pausedHelloMs = 0;
    uint32_t now = millis();
    if (now - pausedHelloMs > 1000) { sendHello(); pausedHelloMs = now; }
    return;  // skip RFID scanning & audio engine entirely
  } else if (wasPaused) {
    wasPaused = false;
    // keep ring red until a valid ACK arrives
  }

  // ---- NORMAL ACTIVE LOOP ----
  const uint32_t now = millis();

  // HELLO ping (light load during active play)
  if (!holdActive && (now - lastHelloMs > 2000)) { sendHello(); lastHelloMs = now; }

  // RFID presence handling
  const bool present = isAnyCardPresent(rfid);

  // ARRIVAL
  if (present && !tagPresent) {
    if (readUid(rfid, currentUid)) {
      tagPresent    = true;
      absentStartMs = 0;
      carried       = 0;
      sendHoldStart(currentUid);
      fillRing(GREEN);   // optimistic cue while waiting for ACK
    }
  }

  // MAINTAINED
  if (present && tagPresent) {
    absentStartMs = 0;
  }

  // REMOVAL (debounced)
  if (!present && tagPresent) {
    if (absentStartMs == 0) absentStartMs = now;
    else if (now - absentStartMs > ABSENCE_MS) {
      tagPresent = false;
      sendHoldStop();
      fillRing(RED);
      stopAudio();
      absentStartMs = 0;
    }
  }

  // Audio keep-alive (only while a hold is accepted)
  if (gameActive && holdActive) {
    handleAudio();
  } else if (playing) {
    stopAudio();
  }
}
