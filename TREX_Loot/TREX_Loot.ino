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
#include <Preferences.h>          // ← NVS for identity
#include "esp_ota_ops.h"

#if TREX_AUDIO_PROGMEM
  #include <AudioFileSourcePROGMEM.h>
  #include "replenish.h"          // const … PROGMEM array + length
#else
  #include <AudioFileSourceLittleFS.h>
  #include <AudioFileSourceBuffer.h>
  constexpr char CLIP_PATH[] = "/replenish.wav";
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
const uint32_t OFF     = 0;

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

static bool     fullAnnounced     = false;
static uint32_t nextGaugeDrawAtMs = 0;

/* ── misc ────────────────────────────────────────────── */
uint16_t g_seq = 1;            // outgoing message seq
uint32_t lastHelloMs = 0;

// Set true once we’ve received the first STATION_UPDATE/ACK/TICK for this game
static bool stationInited = false;

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
  // Re-initialize the PROGMEM source in-place so isOpen()==true
  wavSrc = new (wavSrcStorage) AudioFileSourcePROGMEM(replenish_wav, replenish_wav_len);
#else
  if (wavBuf)  { delete wavBuf;  wavBuf  = nullptr; }
  if (wavFile) { delete wavFile; wavFile = nullptr; }
  wavFile = new AudioFileSourceLittleFS(CLIP_PATH);
  if (!wavFile) { Serial.println("[LOOT] wavFile alloc fail"); return false; }
  wavBuf = new AudioFileSourceBuffer(wavFile, 4096);
  if (!wavBuf) { Serial.println("[LOOT] wavBuf alloc fail"); return false; }
#endif

  // Fresh decoder each time avoids any stale internal state
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
  if (!decoder->loop()) {       // EOF or starvation
    decoder->stop();
    playing = openChain();      // reopen and continue
    if (!playing) Serial.println("[LOOT] audio re-begin failed");
  }
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

/* ── LED drawing ─────────────────────────────────────── */
inline void pumpAudio() { handleAudio(); }  // feed while we block on LED shows

inline uint32_t gaugeColor() {
  return (g_lightState == LightState::GREEN) ? GREEN : RED;
}

void drawRingCarried(uint8_t cur, uint8_t maxC) {
  if (fullBlinkActive || otaInProgress) return;  // OTA owns the ring
  pumpAudio();
  const uint16_t n = ring.numPixels();
  uint16_t lit = 0;
  if (maxC > 0) lit = (uint16_t)((uint32_t)cur * n + (maxC-1)) / maxC;  // ceil
  for (uint16_t i=0;i<n;i++) ring.setPixelColor(i, (i<lit) ? GREEN : OFF);
  ring.show();
}

void drawGaugeInventory(uint16_t inventory, uint16_t capacity) {
  if (otaInProgress) return;
  const LightState colorState = g_lightState;
  if (inventory == lastInvPainted &&
      capacity  == lastCapPainted &&
      colorState == lastGaugeColor) return;

  pumpAudio();

  // 1:1 mapping — each loot lights one LED on the gauge
  uint16_t lit = (inventory > GAUGE_LEN) ? GAUGE_LEN : inventory;

  uint32_t col = RED;
  if      (colorState == LightState::GREEN)  col = GREEN;
  else if (colorState == LightState::YELLOW) col = YELLOW;

  for (uint16_t i = 0; i < GAUGE_LEN; ++i) {
    gauge.setPixelColor(i, (i < lit) ? col : OFF);
    if ((i & 7) == 0) pumpAudio();       // NEW: gentle feed during long loops
  }
  gauge.show();
  pumpAudio();                            // NEW: one more after show

  lastInvPainted = inventory;
  lastCapPainted = capacity;              // tracked for cache only
  lastGaugeColor = colorState;
}

void fillRing(uint32_t c) {
  pumpAudio();
  for (uint16_t i=0;i<ring.numPixels();++i) ring.setPixelColor(i,c);
  ring.show();
}

void fillGauge(uint32_t c) {
  pumpAudio();
  for (uint16_t i=0;i<GAUGE_LEN;++i) gauge.setPixelColor(i,c);
  gauge.show();
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
    while (millis() - t < 140);

    // OFF
    fillGauge(OFF);
    digitalWrite(PIN_MOSFET, LOW);
    t = millis();
    while (millis() - t < 100);
  }
  // Final state: fully off
  fillGauge(OFF);
  digitalWrite(PIN_MOSFET, LOW);
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
  p->holdId    = holdId;
  p->uid       = uid;
  p->stationId = STATION_ID;
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

/* ── RX handler ─────────────────────────────────────── */
void onRx(const uint8_t* data, uint16_t len) {
  if (len < sizeof(MsgHeader)) return;
  auto* h = (const MsgHeader*)data;
  if (h->version != TREX_PROTO_VERSION) return;

  switch ((MsgType)h->type) {
    case MsgType::STATE_TICK: {
      if (h->payloadLen != sizeof(StateTickPayload)) break;
      auto* p = (const StateTickPayload*)(data + sizeof(MsgHeader));
      if      (p->state == (uint8_t)LightState::GREEN)  g_lightState = LightState::GREEN;
      else if (p->state == (uint8_t)LightState::YELLOW) g_lightState = LightState::YELLOW;
      else                                              g_lightState = LightState::RED;

      if (g_lightState == LightState::RED && !holdActive && !tagPresent && !fullBlinkActive && !otaInProgress) {
        fillRing(RED);
      }

      // Don’t clear pipes to 0 until we know the inventory
      if (stationInited) drawGaugeInventory(inv, cap);
      break;
    }

    case MsgType::LOOT_HOLD_ACK: {
      if (h->payloadLen != sizeof(LootHoldAckPayload)) break;
      auto* p = (const LootHoldAckPayload*)(data + sizeof(MsgHeader));
      if (p->holdId != holdId) break;

      // Update state from ACK
      maxCarry = p->maxCarry;
      carried  = (p->carried > maxCarry) ? maxCarry : p->carried;  // clamp
      inv      = p->inventory;
      cap      = p->capacity;
      stationInited = true;

      if (p->accepted) {
        holdActive = true;
        startAudio();                 // begin hold audio
        pumpAudio();

        if (carried >= maxCarry) {
          if (!fullAnnounced || blinkHoldId != p->holdId) {
            startFullBlinkImmediate();   // one-shot full indicator
            fullAnnounced = true;
            blinkHoldId   = p->holdId;
          }
        } else {
          if (fullBlinkActive) stopFullBlink();
          fullAnnounced = false;
          drawRingCarried(carried, maxCarry);
        }

        // Throttled gauge paint (inventory shown 1:1 in drawGaugeInventory)
        uint32_t now = millis();
        if ((int32_t)(now - nextGaugeDrawAtMs) >= 0) {
          drawGaugeInventory(inv, cap);
          nextGaugeDrawAtMs = now + 20;   // ~50 Hz cap
        }
      } else {
        // Not accepted -> ensure visuals sane; don't spam audio
        holdActive = false;
        stopAudio();
        pumpAudio();

        if (carried >= maxCarry) {
          if (!fullAnnounced || blinkHoldId != p->holdId) {
            startFullBlinkImmediate();
            fullAnnounced = true;
            blinkHoldId   = p->holdId;
          }
          uint32_t now = millis();
          if ((int32_t)(now - nextGaugeDrawAtMs) >= 0) {
            drawGaugeInventory(inv, cap);
            nextGaugeDrawAtMs = now + 20;
          }
        } else {
          fullAnnounced = false;
          stopFullBlink();
          fillRing(RED);
        }
      }
      break;
    }

    case MsgType::LOOT_TICK: {
      if (h->payloadLen != sizeof(LootTickPayload)) break;
      auto* p = (const LootTickPayload*)(data + sizeof(MsgHeader));
      if (!holdActive || p->holdId != holdId) break;

      carried = (p->carried > maxCarry) ? maxCarry : p->carried;  // clamp
      inv     = p->inventory;
      stationInited = true;

      if (carried >= maxCarry) {
        if (!fullAnnounced || blinkHoldId != p->holdId) {
          startFullBlinkImmediate();      // one-shot on first time hitting full
          fullAnnounced = true;
          blinkHoldId   = p->holdId;
        }
      } else {
        if (fullBlinkActive) stopFullBlink();
        fullAnnounced = false;
        drawRingCarried(carried, maxCarry);
      }

      uint32_t now = millis();
      if ((int32_t)(now - nextGaugeDrawAtMs) >= 0) {
        drawGaugeInventory(inv, cap);     // paints inventory 1:1 with LEDs
        nextGaugeDrawAtMs = now + 20;
      }

      pumpAudio();
      break;
    }

    case MsgType::HOLD_END: {
      if (h->payloadLen != sizeof(HoldEndPayload)) break;
      auto* p = (const HoldEndPayload*)(data + sizeof(MsgHeader));
      if (p->holdId != holdId) break;

      holdActive = false;
      holdId     = 0;
      stopAudio();
      pumpAudio();

      fullAnnounced = false;              // reset one-shot guard

      if (tagPresent && carried >= maxCarry) {
        if (!fullBlinkActive) startFullBlinkImmediate();
      } else {
        stopFullBlink();
        fillRing(RED);
      }
      // Gauge already reflects latest inv from final TICK; no repaint needed here
      break;
    }

    case MsgType::STATION_UPDATE: {
      if (h->payloadLen != sizeof(StationUpdatePayload)) break;
      auto* p = (const StationUpdatePayload*)(data + sizeof(MsgHeader));
      if (p->stationId == STATION_ID) {
        inv = p->inventory; cap = p->capacity;
        stationInited = true;                // <-- got our initial numbers
        if (!holdActive && !tagPresent) drawGaugeInventory(inv, cap);
      }
      Serial.printf("[STATION_UPDATE] id=%u inv=%u cap=%u\n", p->stationId, inv, cap);
      break;
    }

    case MsgType::GAME_START: {
      gameActive = true;
      wasPaused = false;
      fullBlinkActive = false;
      fullAnnounced = false;

      stationInited = false;                  // wait for fresh inventory before painting

      digitalWrite(PIN_MOSFET, HIGH);         // lamp ON for live round
      stopFullBlink();
      fillRing(RED);                          // ring “awake” in RED

      // IMPORTANT: do NOT call drawGaugeInventory() here,
      // so we don’t blank to 0 before STATION_UPDATE arrives.
      Serial.println("[LOOT] GAME_START");
      break;
    }

    case MsgType::GAME_OVER: {
      gameActive = false; holdActive = false; holdId = 0; fullBlinkActive = false;
      carried = 0; tagPresent = false; absentStartMs = 0;
      stopAudio();
      if (!otaInProgress) fillRing(RED);
      gameOverBlinkAndOff();
      Serial.println("[LOOT] GAME_OVER");
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

    default: break;
  }
}

/* ── setup ───────────────────────────────────────────── */
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n[LOOT] Boot");

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
  i2sOut->SetGain(1.0f);

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
    }
    static uint32_t pausedHelloMs = 0;
    uint32_t now = millis();
    if (now - pausedHelloMs > 1000) { sendHello(); pausedHelloMs = now; }
    return;
  } else if (wasPaused) {
    wasPaused = false;
  }

  // ---- NORMAL ACTIVE LOOP ----
  const uint32_t now = millis();

  if (!holdActive && (now - lastHelloMs > 2000)) { sendHello(); lastHelloMs = now; }

  const bool present = isAnyCardPresent(rfid);

  // ARRIVAL
  if (present && !tagPresent) {
    if (readUid(rfid, currentUid)) {
      tagPresent    = true;
      absentStartMs = 0;
      carried       = 0;
      stopFullBlink();
      sendHoldStart(currentUid);
      fillRing(GREEN);  // optimistic until ACK
    }
  }

  // MAINTAINED
  if (present && tagPresent) { absentStartMs = 0; }

  // REMOVAL (debounced)
  if (!present && tagPresent) {
    if (absentStartMs == 0) absentStartMs = now;
    else if (now - absentStartMs > ABSENCE_MS) {
      tagPresent = false;
      sendHoldStop();
      stopAudio();
      stopFullBlink();
      fillRing(RED);
      absentStartMs = 0;
    }
  }

  // Audio keep-alive (only while a hold is accepted)
  if (gameActive && holdActive) handleAudio();
  else if (playing) stopAudio();

  tickFullBlink();
}
