/*
  TREX – Drop-off Station (Feather S3 + 4×RC522 + 4×Rings + 2×Gauges + MAX98357)
  -------------------------------------------------------------------------------
  • RC522 (x4)   CS: {5,14,18,17}  RST: 11   SPI: SCK=36 MISO=37 MOSI=35
  • 14-px rings  GPIO: {33, 38, 1, 3}
  • 85-px gauges GPIO: {7, 10}
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

#if TREX_AUDIO_PROGMEM
  #include <AudioFileSourcePROGMEM.h>
  #include "LootDrop.h"  // const … PROGMEM array + length
#else
  #include <LittleFS.h>
  #include <AudioFileSourceLittleFS.h>
  #include <AudioFileSourceBuffer.h>
  constexpr char CLIP_PATH[] = "/LootDrop.wav";
#endif

#include <TrexProtocol.h>
#include <TrexTransport.h>
#include "TrexMaintenance.h"

#define WIFI_SSID   "GUD"
#define WIFI_PASS   "EscapE66"
// Use a unique host per device:
#define HOSTNAME    "Drop-off"   // on Drop-off use "trex-drop-6"

/* ── IDs & radio ───────────────────────────────────────────── */
constexpr uint8_t  STATION_ID    = 6;    // drop-off station id
constexpr uint8_t  WIFI_CHANNEL  = 6;    // must match T-Rex

/* ── LEDs / gauges ────────────────────────────────────────── */
constexpr uint16_t GAUGE_LEN        = 85;
constexpr uint8_t  RING_BRIGHTNESS  = 64;
constexpr uint8_t  GAUGE_BRIGHTNESS = 64;

/* Map teamScore → LEDs; set TEAM_GOAL to “full bar” score */
constexpr uint32_t TEAM_GOAL = GAUGE_LEN;  // 1:1 mapping by default

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
bool               playing = false;

/* colours */
static inline uint32_t C(uint8_t r,uint8_t g,uint8_t b){ return Adafruit_NeoPixel::Color(r,g,b); }
const uint32_t RED   = C(255,0,0);
const uint32_t GREEN = C(0,255,0);
const uint32_t WHITE = C(255,255,255);
const uint32_t OFF   = 0;

/* tag tracking */
constexpr uint32_t ABSENCE_MS  = 150;
bool     tagPresent[4] = {0};
uint32_t absentMs[4]   = {0};

/* game/broadcast state */
volatile bool gameActive = true;    // onRx flips this
bool          wasPaused  = false;
volatile LightState    g_lightState = LightState::GREEN;
volatile uint32_t      teamScore  = 0;

/* misc */
uint16_t g_seq = 1;
uint32_t lastHelloMs = 0;

/* ── audio helpers ────────────────────────────────────────── */
static bool openChain() {
  if (decoder && decoder->isRunning()) decoder->stop();

#if TREX_AUDIO_PROGMEM
  if (wavSrc) { delete wavSrc; wavSrc = nullptr; }
  wavSrc = new AudioFileSourcePROGMEM(LootDrop_wav, LootDrop_wav_len);
#else
  if (wavBuf)  { delete wavBuf;  wavBuf = nullptr; }
  if (wavFile) { delete wavFile; wavFile = nullptr; }
  wavFile = new AudioFileSourceLittleFS(CLIP_PATH);
  if (!wavFile) { Serial.println("[DROP] wavFile alloc fail"); return false; }
  // 4 KB buffer to smooth over LED .show() stalls
  wavBuf = new AudioFileSourceBuffer(wavFile, 4096);
  if (!wavBuf) { Serial.println("[DROP] wavBuf alloc fail"); return false; }
#endif

  if (!decoder) decoder = new AudioGeneratorWAV();
#if TREX_AUDIO_PROGMEM
  bool ok = decoder->begin(wavSrc, i2sOut);
#else
  bool ok = decoder->begin(wavBuf, i2sOut);
#endif
  if (!ok) Serial.println("[DROP] decoder.begin() failed");
  return ok;
}

inline void pumpAudio() { if (playing && decoder) decoder->loop(); }

bool startAudio() { if (playing) return true; playing = openChain(); return playing; }
void stopAudio()  { if (!playing) return; decoder->stop(); playing = false; }
inline void handleAudio() {
  if (!playing || !decoder) return;
  if (!decoder->loop()) { decoder->stop(); playing = openChain(); if (!playing) Serial.println("[DROP] audio re-begin failed"); }
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
    if ((p & 3) == 0) pumpAudio();      // feed every ~4 pixels
  }
  pumpAudio();
  ring[idx].show();
  pumpAudio();
}

void drawTeamGauges(uint32_t score) {
  uint16_t lit = 0;
  if (TEAM_GOAL > 0) {
    uint32_t scaled = (uint32_t)score * GAUGE_LEN + (TEAM_GOAL - 1);
    lit = (uint16_t)min<uint32_t>(GAUGE_LEN, scaled / TEAM_GOAL);
  }
  for (auto &g : gauge) {
    for (uint16_t i = 0; i < GAUGE_LEN; ++i) {
      g.setPixelColor(i, (i < lit) ? WHITE : OFF);
      if ((i & 7) == 0) pumpAudio();    // feed every ~8 pixels
    }
    pumpAudio();
    g.show();
    pumpAudio();
  }
}

/* ── NET: messages ───────────────────────────────────────── */
void sendHello() {
  uint8_t buf[sizeof(MsgHeader)+sizeof(HelloPayload)];
  packHeader((uint8_t)MsgType::HELLO, sizeof(HelloPayload), buf);
  auto* p = (HelloPayload*)(buf + sizeof(MsgHeader));
  p->stationType = (uint8_t)StationType::DROP;
  p->stationId   = STATION_ID;
  p->fwMajor = 0; p->fwMinor = 1;
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
  if (h->version != TREX_PROTO_VERSION) return;

  switch ((MsgType)h->type) {
    case MsgType::STATE_TICK: {
      if (h->payloadLen != sizeof(StateTickPayload)) break;
      auto* p = (const StateTickPayload*)(data + sizeof(MsgHeader));
      g_lightState = (p->state == (uint8_t)LightState::GREEN) ? LightState::GREEN : LightState::RED;
      break;
    }

    case MsgType::DROP_RESULT: {
      if (h->payloadLen != sizeof(DropResultPayload)) break;
      auto* p = (const DropResultPayload*)(data + sizeof(MsgHeader));
      teamScore = p->teamScore;
      drawTeamGauges(teamScore);
      break;
    }

    case MsgType::SCORE_UPDATE: {
      if (h->payloadLen != sizeof(ScoreUpdatePayload)) break;
      auto* p = (const ScoreUpdatePayload*)(data + sizeof(MsgHeader));
      teamScore = p->teamScore;
      drawTeamGauges(teamScore);
      break;
    }

    case MsgType::GAME_START: {
      gameActive = true;
      Serial.println("[DROP] GAME_START");
      break;
    }

    case MsgType::GAME_OVER: {
      gameActive = false;
      Serial.println("[DROP] GAME_OVER");
      // force visuals/audio to idle; loop() will keep it paused
      for (int i=0;i<4;i++) { tagPresent[i]=false; absentMs[i]=0; fillRing(i, RED); }
      stopAudio();
      break;
    }

    default: break;
  }
}

/* ── setup ───────────────────────────────────────────────── */
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n[DROP] Boot");

  // ---------- Minimal hardware bring-up (LEDs either way; readers only if active) ----------
  // SPI is harmless to init either way; if you prefer, you can guard reader init with (!inMaint)
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);

  // NeoPixels (rings + gauges)
  for (uint8_t i=0; i<4; ++i) { ring[i].begin(); ring[i].setBrightness(RING_BRIGHTNESS); }
  for (auto &g : gauge)       { g.begin(); g.setBrightness(GAUGE_BRIGHTNESS); g.show(); }
  drawTeamGauges(teamScore);  // start at 0

  // ---------- Normal (game) bring-up ----------
  // RC522 readers
  for (auto &r : rfid) r.PCD_Init();

  // I2S audio (no forced SetRate; WAV header sets it)
  i2sOut = new AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S);
  i2sOut->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT);
  i2sOut->SetRate(48000);
  i2sOut->SetGain(1.0f);

#if !TREX_AUDIO_PROGMEM
  // LittleFS is only needed if you're using the FS audio backend
  LittleFS.begin();
  // Optional: sanity check the clip exists
  File f = LittleFS.open(CLIP_PATH, "r");
  if (!f) Serial.println("[DROP] Missing file on LittleFS");
  else { Serial.printf("[DROP] WAV size: %u bytes\n", (unsigned)f.size()); f.close(); }
#endif

  // ESP-NOW transport (must match channel with T-Rex)
  TransportConfig cfg{ /*maintenanceMode=*/false, /*wifiChannel=*/WIFI_CHANNEL };
  if (!Transport::init(cfg, onRx)) {
    Serial.println("[DROP] Transport init FAILED");
    while (1) delay(1000);
  }

  Serial.printf("Trex proto ver: %d\n", TREX_PROTO_VERSION);

  // Initial LED state: rings RED until a tag arrives; gauges already drawn
  for (uint8_t i=0; i<4; ++i) fillRing(i, RED);
}

/* ── loop ────────────────────────────────────────────────── */
void loop() {
  // Maintenance: long-press BOOT at any time to switch modes
  static Maint::Config mcfg{WIFI_SSID, WIFI_PASS, HOSTNAME,
                            /*apFallback=*/true, /*apChannel=*/WIFI_CHANNEL,
                            /*apPass=*/"trexsetup", /*buttonPin=*/0, /*holdMs=*/1500};
  mcfg.stationType = StationType::DROP;
  mcfg.stationId   = STATION_ID;
  mcfg.enableBeacon = true;

  // If user long-presses BOOT, jump into maintenance and pause game
  if (Maint::checkRuntimeEntry(mcfg)) {
    // visual cue while in maintenance
    const uint32_t BLUE = Adafruit_NeoPixel::Color(0,0,255);
    for (uint8_t i=0;i<4;i++) fillRing(i, BLUE);
    // Run OTA/Telnet/Beacon loop and skip game logic
    Maint::loop();
    return;
  }

  // If already in maintenance (e.g., after OTA), keep servicing it
  if (Maint::active) { Maint::loop(); return; }

  Transport::loop();

  // ---- PAUSED / GAME OVER: only listen for messages ----
  if (!gameActive) {
    if (!wasPaused) {
      wasPaused = true;
      // clear latches; rings red; audio off
      for (int i=0;i<4;i++) { tagPresent[i]=false; absentMs[i]=0; fillRing(i, RED); }
      if (playing) stopAudio();
    }
    static uint32_t pausedHelloMs = 0;
    uint32_t now = millis();
    if (now - pausedHelloMs > 1000) { sendHello(); pausedHelloMs = now; }
    return;  // skip reader scan & audio engine
  } else if (wasPaused) {
    wasPaused = false;
  }

  // ---- ACTIVE ----
  const uint32_t now = millis();

  // HELLO ping (light)
  if (now - lastHelloMs > 2000) { sendHello(); lastHelloMs = now; }

  bool anyActive = false;

  // Scan 4 readers
  for (uint8_t i=0;i<4;i++) {
    pumpAudio();
    MFRC522 &rd = rfid[i];
    bool present = cardPresent(rd);
    if (present) anyActive = true;

    // ARRIVAL
    if (present && !tagPresent[i]) {
      TrexUid uid{};
      if (readUid(rd, uid)) {
        sendDropRequest(uid, i);
        fillRing(i, GREEN);
        tagPresent[i] = true;
      }
      absentMs[i] = 0;
    }

    // MAINTAINED
    if (present && tagPresent[i]) {
      absentMs[i] = 0;
    }

    // REMOVAL (debounced)
    if (!present && tagPresent[i]) {
      if (absentMs[i] == 0) absentMs[i] = now;
      else if (now - absentMs[i] > ABSENCE_MS) {
        fillRing(i, RED);
        tagPresent[i] = false;
        absentMs[i] = 0;
      }
    }
    pumpAudio();
  }

  // Audio: loop while any reader has a tag; stop shortly after last leaves
  static uint32_t lastActiveMs = 0;
  constexpr uint32_t AUDIO_GRACE_MS = 150;
  if (anyActive) lastActiveMs = now;

  if (gameActive && anyActive) {
    if (!playing) startAudio();
    handleAudio();
  } else {
    if (playing && (now - lastActiveMs > AUDIO_GRACE_MS)) stopAudio();
  }
}
