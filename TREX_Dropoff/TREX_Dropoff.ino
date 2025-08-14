/*
  TREX – Drop-off Station (Feather S3 + 4×RC522 + 4×Rings + 2×Gauges + MAX98357)
  -------------------------------------------------------------------------------
  • RC522 (x4)   CS: {5,14,18,17}  RST: 11   SPI: SCK=36 MISO=37 MOSI=35
  • 14-px rings  GPIO: {33, 38, 1, 3}
  • 85-px gauges GPIO: {7, 10}
  • I²S audio    BCLK 43 | LRCLK 44 | DOUT 12  (48 kHz WAV)
  • Transport    ESP-NOW via TrexTransport (channel must match T-Rex)
*/

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_NeoPixel.h>
#include <LittleFS.h>
#include <AudioFileSourceLittleFS.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>

#include <TrexProtocol.h>
#include <TrexTransport.h>

/* ── IDs & radio ───────────────────────────────────────────── */
constexpr uint8_t  STATION_ID    = 6;   // drop-off station id
constexpr uint8_t  WIFI_CHANNEL  = 6;   // must match T-Rex

/* ── LEDs ─────────────────────────────────────────────────── */
constexpr uint16_t GAUGE_LEN        = 85;
constexpr uint8_t  RING_BRIGHTNESS  = 64;
constexpr uint8_t  GAUGE_BRIGHTNESS = 64;

/* Team score → gauge mapping.
   If your run’s target score is different, set TEAM_GOAL accordingly. */
constexpr uint32_t TEAM_GOAL = GAUGE_LEN;   // simple 1:1 mapping by default

/* ── pins ─────────────────────────────────────────────────── */
constexpr uint8_t PIN_SCK   = 36, PIN_MISO = 37, PIN_MOSI = 35, PIN_RST = 11;
constexpr uint8_t CS_PINS[4]   = { 5, 14, 18, 17 };
constexpr uint8_t RING_PINS[4] = { 33, 38,  1,  3 };
constexpr uint8_t GAUGE_PINS[2]= {  7, 10 };

constexpr int PIN_I2S_BCLK  = 43;
constexpr int PIN_I2S_LRCLK = 44;
constexpr int PIN_I2S_DOUT  = 12;
constexpr char CLIP_PATH[]  = "/replenish.wav";

/* ── objects ──────────────────────────────────────────────── */
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

/* audio */
AudioFileSourceLittleFS *wavSrc = nullptr;
AudioGeneratorWAV       *decoder= nullptr;
AudioOutputI2S          *i2sOut = nullptr;
bool                     playing = false;

/* colours */
static inline uint32_t C(uint8_t r,uint8_t g,uint8_t b){ return Adafruit_NeoPixel::Color(r,g,b); }
const uint32_t RED   = C(255,0,0);
const uint32_t GREEN = C(0,255,0);
const uint32_t WHITE = C(255,255,255);
const uint32_t OFF   = 0;

/* tag tracking */
bool      tagPresent[4]  = {0};
uint32_t  absentStart[4] = {0};
constexpr uint32_t ABSENCE_MS = 150;

/* team score (server-auth) */
uint32_t teamScore = 0;

/* tx helpers */
uint16_t g_seq = 1;
static void packHeader(uint8_t type, uint16_t payLen, uint8_t* buf) {
  auto* h = (MsgHeader*)buf;
  h->version      = TREX_PROTO_VERSION;
  h->type         = type;
  h->srcStationId = STATION_ID;
  h->flags        = 0;
  h->payloadLen   = payLen;
  h->seq          = g_seq++;
}

/* UID helpers */
static TrexUid makeUid(const MFRC522& m) {
  TrexUid u{}; u.len = m.uid.size;
  for (uint8_t i=0;i<u.len && i<10;i++) u.bytes[i] = m.uid.uidByte[i];
  return u;
}
static bool isAnyCardPresent(MFRC522 &m) {
  byte atqa[2], len=2;
  return m.PICC_WakeupA(atqa, &len) == MFRC522::STATUS_OK;
}

/* draw helpers */
void fillRing(uint8_t idx, uint32_t c) {
  for (uint16_t p=0;p<ring[idx].numPixels();++p) ring[idx].setPixelColor(p,c);
  ring[idx].show();
}
void drawTeamGauges(uint32_t score) {
  // Map teamScore (0..TEAM_GOAL) → 0..GAUGE_LEN (clamped)
  uint16_t lit = 0;
  if (TEAM_GOAL > 0) {
    uint32_t scaled = (uint32_t)score * GAUGE_LEN + (TEAM_GOAL-1);
    lit = (uint16_t)min<uint32_t>(GAUGE_LEN, scaled / TEAM_GOAL);
  }
  for (auto &g : gauge) {
    for (uint16_t i=0;i<GAUGE_LEN;i++) g.setPixelColor(i, (i<lit)? WHITE : OFF);
    g.show();
  }
}

/* audio */
inline void pumpAudio(){ if (playing && decoder) decoder->loop(); }
bool startAudio() {
  if (playing) return true;
  wavSrc  = new AudioFileSourceLittleFS(CLIP_PATH);
  decoder = new AudioGeneratorWAV();
  if (decoder && wavSrc && decoder->begin(wavSrc, i2sOut)) {
    i2sOut->SetRate(48000);
    playing = true;
    return true;
  }
  if (decoder) { delete decoder; decoder=nullptr; }
  if (wavSrc)  { delete wavSrc;  wavSrc=nullptr; }
  return false;
}
void stopAudio() {
  if (!playing) return;
  decoder->stop();
  delete decoder; decoder=nullptr;
  delete wavSrc;  wavSrc=nullptr;
  playing = false;
}

/* NET: messages */
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

/* RX handler */
void onRx(const uint8_t* data, uint16_t len) {
  if (len < sizeof(MsgHeader)) return;
  auto* h = (const MsgHeader*)data;
  if (h->version != TREX_PROTO_VERSION) return;

  switch ((MsgType)h->type) {
    case MsgType::DROP_RESULT: {
      if (h->payloadLen != sizeof(DropResultPayload)) break;
      auto* p = (const DropResultPayload*)(data + sizeof(MsgHeader));
      teamScore = p->teamScore;
      drawTeamGauges(teamScore);
      // (Optional) flash all rings briefly to celebrate
      for (int i=0;i<4;i++) fillRing(i, WHITE);
      delay(50);
      for (int i=0;i<4;i++) fillRing(i, tagPresent[i]? GREEN : RED);
      break;
    }
    case MsgType::SCORE_UPDATE: {
      if (h->payloadLen != sizeof(ScoreUpdatePayload)) break;
      auto* p = (const ScoreUpdatePayload*)(data + sizeof(MsgHeader));
      teamScore = p->teamScore;
      drawTeamGauges(teamScore);
      break;
    }
    case MsgType::GAME_OVER: {
      // Freeze visuals; audio off
      for (int i=0;i<4;i++) fillRing(i, RED);
      stopAudio();
      break;
    }
    default: break;
  }
}

/* setup */
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n[DROP] Boot");

  LittleFS.begin();

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);
  for (auto &r : rfid) r.PCD_Init();

  for (uint8_t i=0;i<4;i++) { ring[i].begin(); ring[i].setBrightness(RING_BRIGHTNESS); fillRing(i, RED); }
  for (auto &g : gauge) { g.begin(); g.setBrightness(GAUGE_BRIGHTNESS); g.show(); }
  drawTeamGauges(teamScore);

  i2sOut = new AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S);
  i2sOut->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT);
  i2sOut->SetGain(1.0f);
  i2sOut->SetRate(48000);

  TransportConfig cfg{ /*maintenanceMode=*/false, /*wifiChannel=*/WIFI_CHANNEL };
  if (!Transport::init(cfg, onRx)) {
    Serial.println("[DROP] Transport init FAILED");
    while (1) delay(1000);
  }
}

/* loop */
void loop() {
  Transport::loop();
  pumpAudio();

  static uint32_t lastHelloMs=0;
  uint32_t now = millis();
  if (now - lastHelloMs > 1000) { sendHello(); lastHelloMs = now; }

  bool anyActive = false;

  // Scan each reader
  for (uint8_t i=0;i<4;i++) {
    MFRC522 &rd = rfid[i];
    bool present = isAnyCardPresent(rd);
    if (present) anyActive = true;

    // ARRIVAL
    if (present && !tagPresent[i]) {
      // Read UID for this arrival; if read fails, skip send
      if (rd.PICC_ReadCardSerial()) {
        TrexUid uid = makeUid(rd);
        sendDropRequest(uid, i);
        fillRing(i, GREEN);
        tagPresent[i] = true;
      }
      absentStart[i] = 0;
    }

    // MAINTAINED
    if (present && tagPresent[i]) {
      absentStart[i] = 0;
    }

    // REMOVAL (debounced)
    if (!present && tagPresent[i]) {
      if (absentStart[i] == 0) absentStart[i] = now;
      else if (now - absentStart[i] > ABSENCE_MS) {
        fillRing(i, RED);
        tagPresent[i] = false;
        absentStart[i]= 0;
      }
    }
  }

  // Audio engine
  static uint32_t lastActiveMs = 0;
  constexpr uint32_t AUDIO_GRACE_MS = 150;
  if (anyActive) lastActiveMs = now;

  if (!playing && anyActive) startAudio();

  if (playing) {
    if (!anyActive && (now - lastActiveMs > AUDIO_GRACE_MS)) {
      stopAudio();
    } else {
      if (!decoder->loop()) {    // EOF
        stopAudio();
        if (anyActive) startAudio(); // loop while any tag present
      }
    }
  }
}
