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

#include <TrexProtocol.h>
#include <TrexTransport.h>
#include "TrexMaintenance.h"      // ← maintenance mode (OTA, Telnet, mDNS, HTTP FS)
#include <TrexVersion.h>

#include "Identity.h"
#include "IdentitySerial.h"
#include "Audio.h"
#include "OTA.h"
#include "LootNet.h"
#include "LootRx.h"
#include "LootLeds.h"
#include "LootMini.h"

/* ---------- Wi-Fi (Maintenance / OTA HTTP) ---------- */
const char* WIFI_SSID  = "GUD";
const char* WIFI_PASS  = "EscapE66";
uint8_t     WIFI_CHANNEL = 6;

bool transportReady = false;
bool otaSuccessReportPending = false;   // set if we find /ota.json=success at boot
uint32_t otaSuccessSendAt = 0;

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

/* ── colours ─────────────────────────────────────────── */
static inline uint32_t C_RGB(uint8_t r,uint8_t g,uint8_t b){ return Adafruit_NeoPixel::Color(r,g,b); }
const uint32_t RED     = C_RGB(255,0,0);
const uint32_t GREEN   = C_RGB(0,255,0);
const uint32_t BLUE    = C_RGB(0,0,255);
const uint32_t CYAN    = C_RGB(0,200,255);
const uint32_t YELLOW    = C_RGB(255,180,0);
const uint32_t WHITE   = C_RGB(255,255,255);
const uint32_t OFF     = 0;

/* ── RFID presence debounce ──────────────────────────── */
constexpr uint32_t ABSENCE_MS = 150;   // debounce removal
bool          tagPresent      = false;
uint32_t      absentStartMs   = 0;

// ── Bonus warning blink (last 3s of intermission) ──────────────────────────
constexpr uint32_t BONUS_WARN_MS         = 3000;  // blink when msLeft <= this
constexpr uint32_t BONUS_BLINK_PERIOD_MS = 220;   // ~4.5 Hz

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

bool fullAnnounced = false;

// Set true once we’ve received the first STATION_UPDATE/ACK/TICK for this game
bool stationInited = false;

bool s_isBonusNow = false;
bool g_bonusAtTap = false;

// Mini game
volatile bool mgActive = false;   // minigame owns gauge when true
bool mgTried = false;             // one attempt per station

/* ── OTA campaign state ──────────────────────────────── */
bool      otaInProgress     = false;
bool      otaStartRequested = false;
uint32_t  otaCampaignId     = 0;
uint8_t   otaExpectMajor    = 0, otaExpectMinor = 0;
char      otaUrl[128]       = {0};

// ── Soft staggering to avoid same-ms spikes ──────────────────────────────
// Tweak these if you like; they just separate events in time a bit.
constexpr uint16_t RING_STAGGER_MS   = 0;   // ring can stay at 0 (baseline)
constexpr uint16_t EMPTY_STAGGER_MS  = 70;  // empty-gauge blink toggles 70ms after ring
constexpr uint16_t AUDIO_STOP_STAGGER_MS = 12; // stop audio slightly after a FULL ring starts

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

  // While the minigame is active, it owns the gauge and input
  if (mgActive) {
    // MG logic + visuals
    mgLoop();

    // 🔊 keep audio flowing during the minigame
    if (playing) handleAudio();
    tickScheduledAudio();   // optional, safe

    // keep transport/OTA drip
    Transport::loop();
    if (transportReady && otaSuccessReportPending && millis() >= otaSuccessSendAt) {
      sendOtaStatus(OtaPhase::SUCCESS, 0, 0, 0);
      otaClearFile();
      otaSuccessReportPending = false;
    }
    return;
  }

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
      sendHoldStart(currentUid);
      if (inv == 0) startEmptyBlink(); else stopEmptyBlink();
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

}
