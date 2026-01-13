// =============================================================
// TREX Control Station (Feather S3)
//
// With standardized serial protocol for the main Player Management System (PMS)
//
// Standard protocol lines always start with:
//   !PMS
//
// PMS protocol (v1) ***DO NOT REMOVE***:
//   PMS -> Control:
//     !PMS PING
//     !PMS START level=1        (level accepted for standardization; ignored for TREX)
//     !PMS STOP
//
//   Control -> PMS:
//     !PMS PONG v=1 game=trex role=control
//     !PMS STATUS v=1 state=arming|playing level=1 score=.. lives=.. tleft_ms=.. last_reason=..
//       (STATUS is NOT emitted while idle)
//     !PMS EVENT v=1 name=game_start level=1
//     !PMS EVENT v=1 name=game_end reason=timeup|no_lives|stopped score=.. lives=..
//     !PMS EVENT v=1 name=score delta=.. total=.. bonus=0|1
//     !PMS EVENT v=1 name=life delta=-1 lives=..
//
// Build toggles (compile-time):
//   - PMS_STD_ENABLED: enable/disable PMS protocol support
//   - PMS_DEBUG_SERIAL: when 0, suppress non-!PMS debug/legacy Serial prints (clean PMS output)
//
// =============================================================
#include <Arduino.h>
#include <TrexProtocol.h>
#include <TrexTransport.h>
#include <TrexVersion.h>
#include <Preferences.h>
// =============================================================
// PMS serial protocol toggles
// =============================================================

#ifndef PMS_STD_ENABLED
#define PMS_STD_ENABLED 1
#endif

// 1 = keep legacy/debug Serial prints
// 0 = suppress all non-!PMS prints (clean serial stream for PMS)
#ifndef PMS_DEBUG_SERIAL
#define PMS_DEBUG_SERIAL 0
#endif

// PMS STATUS tick period
#ifndef PMS_STATUS_PERIOD_MS
#define PMS_STATUS_PERIOD_MS 250
#endif

// Consider incoming radio status stale if not updated within this window
#ifndef PMS_STALE_MS
#define PMS_STALE_MS 1000
#endif

// =============================================================
// Debug print helpers (suppressed when PMS_DEBUG_SERIAL=0)
// =============================================================

#if PMS_DEBUG_SERIAL
  #define DBG_PRINT(...)    Serial.print(__VA_ARGS__)
  #define DBG_PRINTLN(...)  Serial.println(__VA_ARGS__)
  #define DBG_PRINTF(...)   Serial.printf(__VA_ARGS__)
#else
  #define DBG_PRINT(...)    do { } while (0)
  #define DBG_PRINTLN(...)  do { } while (0)
  #define DBG_PRINTF(...)   do { } while (0)
#endif


// Radio identity for this station
static uint8_t WIFI_CHANNEL = 6;   // must match server (loaded from NVS)
constexpr uint8_t STATION_ID   = 7;   // unique id for CONTROL station

// Station IDs for targeting
constexpr uint8_t SERVER_STATION_ID   = 0; // T-Rex server
constexpr uint8_t DROPOFF_STATION_ID  = 6; // your Drop-off station id
// Loot stations are typically 1..5

// --- Radio config (persisted in NVS) ------------------------------------
// Keys live in Preferences namespace "trex":
//   chan = Wi-Fi channel (1..13)
//   txf  = TX framed (0/1)
//   rxl  = RX accept legacy (0/1)
static bool TX_FRAMED        = false;  // false = legacy packets (no wire header)
static bool RX_ACCEPT_LEGACY = true;   // true = accept packets without wire header

static volatile bool    gRadioCfgPending = false;
static RadioCfgPayload  gRadioCfgMsg{};

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

  DBG_PRINTF("[RADIO] Loaded: chan=%u txFramed=%u rxLegacy=%u",
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

  DBG_PRINTF("[RADIO] Apply: chan=%u txFramed=%u rxLegacy=%u (rebooting)",
                (unsigned)ch,
                (unsigned)(txf ? 1 : 0),
                (unsigned)(rxl ? 1 : 0));

  saveRadioConfig(ch, txf, rxl);
  delay(150);
  ESP.restart();
}

// Simple snapshot of latest game status from server
struct GameStatusSnapshot {
  bool     hasStatus     = false;
  uint32_t teamScore     = 0;
  uint32_t msLeftGame    = 0;
  uint32_t msLeftRound   = 0;
  uint8_t  roundIndex    = 0;
  uint8_t  phase         = 0;   // Phase enum: 1=PLAYING, 2=END
  uint8_t  lightState    = 0;   // LightState: 0=GREEN,1=RED,2=YELLOW
  uint8_t  livesRemaining= 0;
  uint8_t  livesMax      = 0;
  uint32_t lastUpdateMs  = 0;
};

static GameStatusSnapshot gStatus;
static uint32_t gLastPrintMs   = 0;
static uint16_t gSeq           = 1;   // per-sender sequence
static bool     gServerInMaint = false; // set when we MAINT SERVER/ALL, cleared on GAME_STATUS


// =============================================================
// PMS protocol helpers
// =============================================================

static bool gEndHintStopped = false;   // set when STOP is requested (legacy or !PMS); used once for game_end reason

static uint32_t gLastPmsTickMs      = 0;
static bool     gPmsBaselineValid   = false;
static bool     gPmsLastStatusValid = false;
static bool     gPmsLastWasStale    = true;
static uint8_t  gPmsLastStateKind   = 0;     // 0=idle,1=arming,2=playing
static uint32_t gPmsLastScore       = 0;
static uint8_t  gPmsLastLives       = 0;

static uint8_t pmsStateKind(bool statusValid, uint8_t phase, uint32_t msLeftGame) {
  // We only emit STATUS while the game is actually active.
  // - PLAYING phase is always active
  // - Some servers may report an unknown phase while still counting down game time;
  //   treat that as ARMING (active) only if msLeftGame > 0.
  if (!statusValid) return 0;              // idle
  if (phase == 1) return 2;                // PLAYING
  if (phase == 2) return 0;                // END => idle
  if (msLeftGame > 0) return 1;            // active but not PLAYING => ARMING
  return 0;                                // otherwise idle
}

static const char* pmsStateStr(uint8_t kind) {
  switch (kind) {
    case 2: return "playing";
    case 1: return "arming";
    default: return "idle";
  }
}

static const char* pmsLastReasonStr(bool stale, bool stateChanged, int32_t scoreDelta, int32_t livesDelta) {
  if (stale) return "stale";
  if (livesDelta < 0) return "life";
  if (scoreDelta > 0) return "score";
  if (stateChanged)   return "state";
  return "none";
}

// --- PMS output --------------------------------------------------------

static void pmsPrintPong() {
#if PMS_STD_ENABLED
  Serial.println(F("!PMS PONG v=1 game=trex role=control"));
#endif
}

static void pmsPrintStatus(const char* state,
                           uint8_t level,
                           uint32_t score,
                           uint8_t lives,
                           uint32_t tleftMs,
                           const char* lastReason) {
#if PMS_STD_ENABLED
  Serial.print(F("!PMS STATUS v=1 state="));
  Serial.print(state);
  Serial.print(F(" level="));
  Serial.print(level);
  Serial.print(F(" score="));
  Serial.print((unsigned long)score);
  Serial.print(F(" lives="));
  Serial.print((unsigned)lives);
  Serial.print(F(" tleft_ms="));
  Serial.print((unsigned long)tleftMs);
  Serial.print(F(" last_reason="));
  Serial.println(lastReason);
#endif
}

static void pmsPrintEventGameStart(uint8_t level) {
#if PMS_STD_ENABLED
  Serial.print(F("!PMS EVENT v=1 name=game_start level="));
  Serial.println(level);
#endif
}

static void pmsPrintEventGameEnd(const char* reason, uint32_t score, uint8_t lives) {
#if PMS_STD_ENABLED
  Serial.print(F("!PMS EVENT v=1 name=game_end reason="));
  Serial.print(reason);
  Serial.print(F(" score="));
  Serial.print((unsigned long)score);
  Serial.print(F(" lives="));
  Serial.println((unsigned)lives);
#endif
}

static void pmsPrintEventScore(int32_t delta, uint32_t total, bool bonus) {
#if PMS_STD_ENABLED
  Serial.print(F("!PMS EVENT v=1 name=score delta="));
  Serial.print(delta);
  Serial.print(F(" total="));
  Serial.print((unsigned long)total);
  Serial.print(F(" bonus="));
  Serial.println(bonus ? 1 : 0);
#endif
}

static void pmsPrintEventLife(int32_t delta, uint8_t lives) {
#if PMS_STD_ENABLED
  Serial.print(F("!PMS EVENT v=1 name=life delta="));
  Serial.print(delta);
  Serial.print(F(" lives="));
  Serial.println((unsigned)lives);
#endif
}

// --- PMS input parsing -------------------------------------------------

static int32_t parseKeyInt(const String& line, const char* key, int32_t defaultVal) {
  String pattern = String(key) + "=";
  int idx = line.indexOf(pattern);
  if (idx < 0) return defaultVal;

  idx += pattern.length();
  int end = idx;
  while (end < (int)line.length()) {
    char c = line.charAt(end);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
    end++;
  }

  String val = line.substring(idx, end);
  val.trim();
  if (val.length() == 0) return defaultVal;
  return val.toInt();
}

static String firstToken(const String& s) {
  int sp = s.indexOf(' ');
  if (sp < 0) return s;
  return s.substring(0, sp);
}

static String afterFirstToken(const String& s) {
  int sp = s.indexOf(' ');
  if (sp < 0) return "";
  return s.substring(sp + 1);
}

static void handlePmsCommand(const String& rawLine) {
#if PMS_STD_ENABLED
  String line = rawLine;
  line.trim();
  if (!line.startsWith("!PMS")) return;

  String rest = line.substring(4);
  rest.trim();
  if (rest.length() == 0) return;

  String kind = firstToken(rest);
  String args = afterFirstToken(rest);
  kind.toUpperCase();

  if (kind == "PING") {
    pmsPrintPong();
    return;
  }

  if (kind == "START") {
    (void)parseKeyInt(args, "level", 1); // accepted for standardization; ignored for TREX
    sendControl(ControlOp::START, (uint8_t)StationType::TREX, 0);
    gServerInMaint    = false;
    gEndHintStopped   = false;
    DBG_PRINTLN("OK START");
    return;
  }

  if (kind == "STOP") {
    sendControl(ControlOp::STOP, (uint8_t)StationType::TREX, 0);
    gEndHintStopped = true;
    DBG_PRINTLN("OK STOP");
    return;
  }

  // Unknown PMS command: keep silent in production; optional debug log.
  DBG_PRINT("Unknown PMS cmd: ");
  DBG_PRINTLN(rawLine);
#else
  (void)rawLine;
#endif
}

static void handleSerialLine(const String& rawLine) {
  String line = rawLine;
  line.trim();
  if (line.length() == 0) return;

#if PMS_STD_ENABLED
  if (line.startsWith("!PMS")) {
    handlePmsCommand(line);
    return;
  }
#endif

  // Legacy CLI
  handleCommand(line);
}

// --- PMS STATUS tick + derived EVENTS ---------------------------------

static void pmsTick() {
#if PMS_STD_ENABLED
  uint32_t now = millis();
  if (now - gLastPmsTickMs < PMS_STATUS_PERIOD_MS) return;
  gLastPmsTickMs = now;

  // Snapshot
  bool statusValid = (!gServerInMaint && gStatus.hasStatus);
  bool stale = true;
  if (statusValid) {
    stale = (now - gStatus.lastUpdateMs) > (uint32_t)PMS_STALE_MS;
  }

  uint32_t msLeftGame = statusValid ? gStatus.msLeftGame : 0;

  uint8_t curKind  = pmsStateKind(statusValid, statusValid ? gStatus.phase : 0, msLeftGame);
  const char* curStateStr = pmsStateStr(curKind);

  // Protocol fields
  const uint8_t level = 1; // accepted on START for standardization; currently ignored by TREX
  uint32_t score = statusValid ? gStatus.teamScore : 0;
  uint8_t  lives = statusValid ? gStatus.livesRemaining : 0;
  uint32_t tleft = msLeftGame;

  // Initialize baseline without emitting spurious events.
  if (!gPmsBaselineValid) {
    gPmsBaselineValid   = true;
    gPmsLastStatusValid = statusValid;
    gPmsLastWasStale    = stale;
    gPmsLastStateKind   = curKind;
    gPmsLastScore       = score;
    gPmsLastLives       = lives;

    // Only print STATUS when active (arming/playing). No STATUS while idle.
    if (curKind != 0) {
      const char* lr = stale ? "stale" : "none";
      pmsPrintStatus(curStateStr, level, score, lives, tleft, lr);
    }
    return;
  }

  bool stateChanged = (curKind != gPmsLastStateKind);

  int32_t scoreDelta = (int32_t)score - (int32_t)gPmsLastScore;
  int32_t livesDelta = (int32_t)lives - (int32_t)gPmsLastLives;

  // EVENTS (only when data is fresh)
  // - game_start can be emitted when we first see PLAYING with fresh data
  if (statusValid && !stale) {
    if (curKind == 2 && gPmsLastStateKind != 2) {
      pmsPrintEventGameStart(level);
      gEndHintStopped = false; // clear any stale stop hint
    }
  }

  // Other events require both previous and current snapshots to be fresh & valid
  if (statusValid && gPmsLastStatusValid && !stale && !gPmsLastWasStale) {
    // game_end: leaving PLAYING
    if (gPmsLastStateKind == 2 && curKind != 2) {
      const char* reason = "timeup";
      if (gEndHintStopped) {
        reason = "stopped";
        gEndHintStopped = false;
      } else if (lives == 0) {
        reason = "no_lives";
      }
      pmsPrintEventGameEnd(reason, score, lives);
    }

    // score: ignore negative deltas (resets)
    if (scoreDelta > 0) {
      bool bonus = (scoreDelta > 1);
      pmsPrintEventScore(scoreDelta, score, bonus);
    }

    // life: ignore positive deltas (resets)
    if (livesDelta < 0) {
      pmsPrintEventLife(livesDelta, lives);
    }
  }

  // STATUS (only while active; no STATUS while idle)
  if (curKind != 0) {
    const char* lastReason = pmsLastReasonStr(stale, stateChanged, scoreDelta, livesDelta);
    pmsPrintStatus(curStateStr, level, score, lives, tleft, lastReason);
  }

  // Update baseline
  gPmsLastStatusValid = statusValid;
  gPmsLastWasStale    = stale;
  gPmsLastStateKind   = curKind;
  gPmsLastScore       = score;
  gPmsLastLives       = lives;
#endif
}


// --- Helpers to stringify enums ----------------------------------------

const char* phaseToStr(uint8_t ph) {
  switch (ph) {
    case 1: return "PLAYING";
    case 2: return "END";
    default: return "UNKNOWN";
  }
}

const char* lightToStr(uint8_t ls) {
  switch (ls) {
    case (uint8_t)LightState::GREEN:  return "GREEN";
    case (uint8_t)LightState::RED:    return "RED";
    case (uint8_t)LightState::YELLOW: return "YELLOW";
    default: return "UNKNOWN";
  }
}

// --- Network TX: HELLO + CONTROL_CMD -----------------------------------

void sendHello() {
  uint8_t buf[sizeof(MsgHeader) + sizeof(HelloPayload)];
  auto* h = (MsgHeader*)buf;
  h->version      = TREX_PROTO_VERSION;
  h->type         = (uint8_t)MsgType::HELLO;
  h->srcStationId = STATION_ID;
  h->flags        = 0;
  h->payloadLen   = sizeof(HelloPayload);
  h->seq          = gSeq++;

  auto* p = (HelloPayload*)(buf + sizeof(MsgHeader));
  p->stationType = (uint8_t)StationType::CONTROL;
  p->stationId   = STATION_ID;
  p->fwMajor     = TREX_FW_MAJOR;
  p->fwMinor     = TREX_FW_MINOR;
  p->wifiChannel = WIFI_CHANNEL;
  memset(p->mac, 0, sizeof(p->mac));

  Transport::sendToServer(buf, sizeof(buf));
}

// Always broadcast CONTROL_CMD; targets are encoded in payload
void sendControl(ControlOp op, uint8_t targetType, uint8_t targetId) {
  uint8_t buf[sizeof(MsgHeader) + sizeof(ControlCmdPayload)];
  auto* h = (MsgHeader*)buf;
  h->version      = TREX_PROTO_VERSION;
  h->type         = (uint8_t)MsgType::CONTROL_CMD;
  h->srcStationId = STATION_ID;
  h->flags        = 0;
  h->payloadLen   = sizeof(ControlCmdPayload);
  h->seq          = gSeq++;

  auto* p = (ControlCmdPayload*)(buf + sizeof(MsgHeader));
  p->op         = (uint8_t)op;
  p->targetType = targetType;  // semantics depend on op
  p->targetId   = targetId;    // for CONTROL_CMD: 255 = wildcard for ALL ids
  p->_pad       = 0;

  Transport::broadcast(buf, sizeof(buf));
}

// RADIO_CFG request (CONTROL -> server). Stations will ignore because srcStationId != 0.
void sendRadioCfgRequest(uint8_t wifiChannel, int8_t txFramed = -1, int8_t rxLegacy = -1) {
  uint8_t buf[sizeof(MsgHeader) + sizeof(RadioCfgPayload)];
  auto* h = (MsgHeader*)buf;
  h->version      = TREX_PROTO_VERSION;
  h->type         = (uint8_t)MsgType::RADIO_CFG;
  h->srcStationId = STATION_ID;
  h->flags        = 0;
  h->payloadLen   = sizeof(RadioCfgPayload);
  h->seq          = gSeq++;

  auto* p = (RadioCfgPayload*)(buf + sizeof(MsgHeader));
  p->wifiChannel = wifiChannel;                  // 1..13, or 0 = keep
  p->txFramed    = (txFramed < 0) ? 255 : (uint8_t)txFramed; // 0/1, or 255 = keep
  p->rxLegacy    = (rxLegacy < 0) ? 255 : (uint8_t)rxLegacy; // 0/1, or 255 = keep
  p->_pad        = 0;

  Transport::broadcast(buf, sizeof(buf));
}

// --- Network RX: update snapshot + emit events -------------------------

void onRx(const uint8_t* data, uint16_t len) {
  if (len < sizeof(MsgHeader)) return;
  auto* h = (const MsgHeader*)data;
  if (h->version != TREX_PROTO_VERSION) return;

  const uint8_t* payload = data + sizeof(MsgHeader);

  switch ((MsgType)h->type) {
    case MsgType::RADIO_CFG: {
      if (h->payloadLen != sizeof(RadioCfgPayload)) break;
      if (h->srcStationId != 0) break; // only apply config from server
      const auto* p = (const RadioCfgPayload*)(payload);
      gRadioCfgMsg = *p;
      gRadioCfgPending = true;
      DBG_PRINTF("[RADIO] RADIO_CFG received: chan=%u txFramed=%u rxLegacy=%u\n",
                    (unsigned)p->wifiChannel,
                    (unsigned)p->txFramed,
                    (unsigned)p->rxLegacy);
      break;
    }

    case MsgType::GAME_STATUS: {
      if (h->payloadLen != sizeof(GameStatusPayload)) break;
      auto* p = (const GameStatusPayload*)payload;
      gStatus.hasStatus    = true;
      gStatus.teamScore    = p->teamScore;
      gStatus.msLeftGame   = p->msLeftGame;
      gStatus.msLeftRound  = p->msLeftRound;
      gStatus.roundIndex   = p->roundIndex;
      gStatus.phase        = p->phase;
      gStatus.lightState   = p->lightState;
      gStatus.lastUpdateMs = millis();
      gServerInMaint       = false;   // fresh status => server back from maint
      break;
    }

    case MsgType::LIVES_UPDATE: {
      if (h->payloadLen != sizeof(LivesUpdatePayload)) break;
      auto* p = (const LivesUpdatePayload*)payload;
      gStatus.livesRemaining = p->livesRemaining;
      gStatus.livesMax       = p->livesMax;
      gStatus.hasStatus      = true;
      gStatus.lastUpdateMs   = millis();
      gServerInMaint         = false;

      // Only emit an event when this update corresponds to an actual failure
      if (p->reason != 0) {
        DBG_PRINTF("EVENT LIFE_LOST reason=%u blameSid=%u lives=%u/%u\n",
                      (unsigned)p->reason,
                      (unsigned)p->blameSid,
                      (unsigned)p->livesRemaining,
                      (unsigned)p->livesMax);
      }
      break;
    }

    case MsgType::SCORE_UPDATE: {
      if (h->payloadLen != sizeof(ScoreUpdatePayload)) break;
      auto* p = (const ScoreUpdatePayload*)payload;
      gStatus.teamScore    = p->teamScore;
      gStatus.hasStatus      = true;
      gStatus.lastUpdateMs   = millis();
      gServerInMaint         = false;
      DBG_PRINTF("EVENT SCORE score=%lu\n", (unsigned long)gStatus.teamScore);
      break;
    }

    case MsgType::GAME_OVER: {
      if (h->payloadLen < sizeof(GameOverPayload)) break;
      auto* p = (const GameOverPayload*)payload;
      (void)p;
      gStatus.phase        = 2;  // END
      gStatus.hasStatus      = true;
      gStatus.lastUpdateMs   = millis();
      gServerInMaint         = false;
      uint32_t score = gStatus.teamScore;
      DBG_PRINTF("EVENT GAME_OVER reason=%u score=%lu\n",
                    (unsigned)p->reason, (unsigned long)score);
      break;
    }

    default:
      break;
  }
}

// --- Serial command parsing -------------------------------------------

void handleMaintCommand(const String& cmd) {
  // cmd is already uppercased and trimmed, starts with "MAINT"

  if (cmd.length() == 5) {
    // Just "MAINT" -> treat as "MAINT SERVER"
    sendControl(ControlOp::ENTER_MAINT,
                (uint8_t)StationType::TREX,
                0);         // server only (TREX, id=0)
    gServerInMaint = true;
    DBG_PRINTLN("OK MAINT SERVER");
    return;
  }

  String rest = cmd.substring(5);
  rest.trim();

  if (rest.length() == 0) {
    // same as bare MAINT
    sendControl(ControlOp::ENTER_MAINT,
                (uint8_t)StationType::TREX,
                0);
    gServerInMaint = true;
    DBG_PRINTLN("OK MAINT SERVER");
    return;
  }

  // MAINT SERVER
  if (rest == "SERVER") {
    sendControl(ControlOp::ENTER_MAINT,
                (uint8_t)StationType::TREX,
                0);         // TREX only (server)
    gServerInMaint = true;
    DBG_PRINTLN("OK MAINT SERVER");
    return;
  }

  // MAINT DROP / MAINT DROPOFF
  if (rest == "DROP" || rest == "DROPOFF") {
    sendControl(ControlOp::ENTER_MAINT,
                (uint8_t)StationType::DROP,
                DROPOFF_STATION_ID);   // just the drop-off station
    DBG_PRINTLN("OK MAINT DROPOFF");
    return;
  }

  // MAINT LOOT...
  if (rest.startsWith("LOOT")) {
    String lootRest = rest.substring(4);
    lootRest.trim();

    if (lootRest.length() == 0 || lootRest == "ALL") {
      // MAINT LOOT or MAINT LOOT ALL
      sendControl(ControlOp::ENTER_MAINT,
                  (uint8_t)StationType::LOOT,
                  255);   // 255 = ALL loot ids for CONTROL_CMD
      DBG_PRINTLN("OK MAINT LOOT ALL");
      return;
    }

    // MAINT LOOT N
    int lootId = lootRest.toInt();
    if (lootId <= 0 || lootId > 255) {
      DBG_PRINTLN("ERR MAINT LOOT expects 1..255 or ALL");
      DBG_PRINTLN("     e.g. MAINT LOOT 3   (loot station 3)");
      return;
    }
    sendControl(ControlOp::ENTER_MAINT,
                (uint8_t)StationType::LOOT,
                (uint8_t)lootId);      // specific loot station id
    DBG_PRINT("OK MAINT LOOT ");
    DBG_PRINTLN(lootId);
    return;
  }

  // MAINT ALL -> targetType=255, targetId=255 (true wildcard for CONTROL_CMD)
  if (rest == "ALL") {
    sendControl(ControlOp::ENTER_MAINT,
                255,   // all types (TREX + LOOT + DROP + CONTROL)
                255);  // all ids
    gServerInMaint = true;  // includes server
    DBG_PRINTLN("OK MAINT ALL");
    return;
  }

  DBG_PRINTLN("ERR MAINT usage:");
  DBG_PRINTLN("  MAINT                  (server)");
  DBG_PRINTLN("  MAINT SERVER");
  DBG_PRINTLN("  MAINT DROPOFF");
  DBG_PRINTLN("  MAINT LOOT             (all loot stations)");
  DBG_PRINTLN("  MAINT LOOT ALL");
  DBG_PRINTLN("  MAINT LOOT <N>         (loot station N, 1..5)");
  DBG_PRINTLN("  MAINT ALL              (server + drop-off + all loot)");
}

void handleLootCommand(const String& cmd) {
  // cmd is already uppercased and trimmed, starts with "LOOT"
  // Options:
  //   LOOT           -> LOOT ALL
  //   LOOT ALL
  //   LOOT N         (1..5)

  if (cmd.length() == 4) {
    // bare LOOT -> all loot
    sendControl(ControlOp::LOOT_OTA,
                (uint8_t)StationType::TREX,  // server will orchestrate OTA campaign
                0);                          // targetId=0 => all loot (OtaCampaign side)
    DBG_PRINTLN("OK LOOT ALL");
    return;
  }

  String rest = cmd.substring(4);
  rest.trim();

  if (rest.length() == 0 || rest == "ALL") {
    sendControl(ControlOp::LOOT_OTA,
                (uint8_t)StationType::TREX,
                0);
    DBG_PRINTLN("OK LOOT ALL");
    return;
  }

  int lootId = rest.toInt();
  if (lootId <= 0 || lootId > 255) {
    DBG_PRINTLN("ERR LOOT expects 1..255 or ALL");
    DBG_PRINTLN("     e.g. LOOT 3        (OTA loot station 3 only)");
    return;
  }

  // Here: targetId = loot station id; server will use this to aim the OTA campaign.
  sendControl(ControlOp::LOOT_OTA,
              (uint8_t)StationType::TREX,   // still routed to server
              (uint8_t)lootId);             // which loot id(s) to update
  DBG_PRINT("OK LOOT ");
  DBG_PRINTLN(lootId);
}

void printHelp() {
  DBG_PRINTLN();
  DBG_PRINTLN("=== T-REX CONTROL HELP ===");
  DBG_PRINTLN();

#if PMS_STD_ENABLED
  DBG_PRINTLN("PMS protocol (lines beginning with !PMS):");
  DBG_PRINTLN("  !PMS PING");
  DBG_PRINTLN("  !PMS START level=1");
  DBG_PRINTLN("  !PMS STOP");
  DBG_PRINTLN();
  DBG_PRINTLN("Notes:");
  DBG_PRINTLN("  - When PMS_DEBUG_SERIAL=0, all non-!PMS prints (including this help) are suppressed.");
  DBG_PRINTLN();
#endif

  DBG_PRINTLN("Core game control:");
  DBG_PRINTLN("  START             - Start a new game on the server");
  DBG_PRINTLN("  STOP              - End the current game (manual GAME_OVER)");
  DBG_PRINTLN();

  DBG_PRINTLN("Radio / network (persisted in NVS; applied by server and causes reboot):");
  DBG_PRINTLN("  RADIO             - Print local radio settings (chan/txFramed/rxLegacy)");
  DBG_PRINTLN("  CHAN <1..13>      - Request network channel change (server broadcasts + all reboot)");
  DBG_PRINTLN("  WIRE LEGACY       - Legacy packets (no TRex header), accept legacy");
  DBG_PRINTLN("  WIRE FRAMED       - Add TRex wire header, still accept legacy (safe transition)");
  DBG_PRINTLN("  WIRE STRICT       - Add TRex wire header, reject legacy (max isolation)");
  DBG_PRINTLN();

  DBG_PRINTLN("Maintenance / OTA (enter ArduinoOTA + Telnet, etc.):");
  DBG_PRINTLN("  MAINT             - Maintenance mode on server (same as MAINT SERVER)");
  DBG_PRINTLN("  MAINT SERVER      - Server only (T-Rex logic)");
  DBG_PRINTLN("  MAINT DROPOFF     - Drop-off station only");
  DBG_PRINTLN("  MAINT LOOT        - All loot stations");
  DBG_PRINTLN("  MAINT LOOT ALL    - Same as above");
  DBG_PRINTLN("  MAINT LOOT <N>    - Single loot station (N = 1..255; typical 1..5)");
  DBG_PRINTLN("  MAINT ALL         - Server + Drop-off + all loot stations");
  DBG_PRINTLN();

  DBG_PRINTLN("Loot firmware OTA (via server campaign):");
  DBG_PRINTLN("  LOOT              - OTA ALL loot stations");
  DBG_PRINTLN("  LOOT ALL          - Same as above");
  DBG_PRINTLN("  LOOT <N>          - OTA loot station N only (N = 1..255; typical 1..5)");
  DBG_PRINTLN();

  DBG_PRINTLN("Status / debug:");
  DBG_PRINTLN("  STATUS            - Print one status line immediately");
  DBG_PRINTLN("                       (phase, round, score, msGame, msRound, light, lives)");
  DBG_PRINTLN("  HELP              - Show this help");
  DBG_PRINTLN();

  DBG_PRINTLN("Examples:");
  DBG_PRINTLN("  CHAN 11           -> switch the whole T-Rex network to channel 11");
  DBG_PRINTLN("  WIRE FRAMED       -> enable TRex wire header (still accepts legacy)");
  DBG_PRINTLN("  WIRE STRICT       -> TRex-only framing (ignores other games completely)");
  DBG_PRINTLN("  MAINT SERVER      -> put only the server into OTA/Telnet mode");
  DBG_PRINTLN("  MAINT LOOT 3      -> maintenance for loot station id=3");
  DBG_PRINTLN("  LOOT 2            -> OTA firmware to loot station id=2 only");
  DBG_PRINTLN("  LOOT ALL          -> OTA firmware to all loot stations");
  DBG_PRINTLN("  MAINT ALL         -> maintenance on entire T-Rex network");
  DBG_PRINTLN();
}

void handleCommand(const String& raw) {
  String cmd = raw;
  cmd.trim();
  if (cmd.length() == 0) return;

  cmd.toUpperCase();

  if (cmd == "START") {
    sendControl(ControlOp::START,
                (uint8_t)StationType::TREX,
                0);
    gServerInMaint  = false;
    gEndHintStopped = false;
    DBG_PRINTLN("OK START");
  } else if (cmd == "STOP") {
    sendControl(ControlOp::STOP,
                (uint8_t)StationType::TREX,
                0);
    gEndHintStopped = true;
    DBG_PRINTLN("OK STOP");
  } else if (cmd.startsWith("MAINT")) {
    handleMaintCommand(cmd);
  } else if (cmd.startsWith("LOOT")) {
    handleLootCommand(cmd);
  } else if (cmd.startsWith("CHAN")) {
    // CHAN N  (1..13)
    String rest = cmd.substring(4);
    rest.trim();
    int ch = rest.toInt();
    if (ch >= 1 && ch <= 13) {
      sendRadioCfgRequest((uint8_t)ch);
      DBG_PRINTF("OK CHAN %d (requested)\n", ch);
    } else {
      DBG_PRINTLN("ERR CHAN expects 1..13");
    }
  } else if (cmd.startsWith("WIRE")) {
    String mode = cmd.substring(4);
    mode.trim();
    if (mode == "LEGACY") {
      sendRadioCfgRequest(/*wifiChannel=*/0, /*txFramed=*/0, /*rxLegacy=*/1);
      DBG_PRINTLN("OK WIRE LEGACY (requested)");
    } else if (mode == "FRAMED") {
      sendRadioCfgRequest(/*wifiChannel=*/0, /*txFramed=*/1, /*rxLegacy=*/1);
      DBG_PRINTLN("OK WIRE FRAMED (requested)");
    } else if (mode == "STRICT") {
      sendRadioCfgRequest(/*wifiChannel=*/0, /*txFramed=*/1, /*rxLegacy=*/0);
      DBG_PRINTLN("OK WIRE STRICT (requested)");
    } else {
      DBG_PRINTLN("ERR WIRE expects LEGACY | FRAMED | STRICT");
    }
  } else if (cmd == "RADIO" || cmd == "RADIO?") {
    DBG_PRINTF("RADIO chan=%u txFramed=%u rxLegacy=%u\n",
                  (unsigned)WIFI_CHANNEL,
                  (unsigned)(TX_FRAMED ? 1 : 0),
                  (unsigned)(RX_ACCEPT_LEGACY ? 1 : 0));
  } else if (cmd == "STATUS") {
    if (gServerInMaint) {
      DBG_PRINTF("STATUS phase=MAINT score=%lu\n",
                    (unsigned long)gStatus.teamScore);
    } else if (!gStatus.hasStatus) {
      DBG_PRINTLN("STATUS phase=UNKNOWN score=0");
    } else {
      DBG_PRINTF("STATUS phase=%s round=%u score=%lu msGame=%lu msRound=%lu light=%s lives=%u/%u\n",
                    phaseToStr(gStatus.phase),
                    (unsigned)gStatus.roundIndex,
                    (unsigned long)gStatus.teamScore,
                    (unsigned long)gStatus.msLeftGame,
                    (unsigned long)gStatus.msLeftRound,
                    lightToStr(gStatus.lightState),
                    (unsigned)gStatus.livesRemaining,
                    (unsigned)gStatus.livesMax);
    }
  } else if (cmd == "HELP") {
    printHelp();
  } else {
    DBG_PRINT("ERR unknown command: ");
    DBG_PRINTLN(cmd);
    DBG_PRINTLN("Type HELP for a list of commands.");
  }
}

// --- Periodic STATUS print --------------------------------------------

void printPeriodicStatus() {
  if (gServerInMaint) return;
  if (!gStatus.hasStatus) return;
  if (gStatus.phase != 1) return; // only while PLAYING

  DBG_PRINTF("STATUS phase=%s round=%u score=%lu msGame=%lu msRound=%lu light=%s lives=%u/%u\n",
                phaseToStr(gStatus.phase),
                (unsigned)gStatus.roundIndex,
                (unsigned long)gStatus.teamScore,
                (unsigned long)gStatus.msLeftGame,
                (unsigned long)gStatus.msLeftRound,
                lightToStr(gStatus.lightState),
                    (unsigned)gStatus.livesRemaining,
                    (unsigned)gStatus.livesMax);
}

// --- Arduino setup/loop -----------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(50);
  DBG_PRINTLN("\n[TREX_CTRL] Control station boot");

  loadRadioConfig();

  TransportConfig cfg;
  cfg.maintenanceMode = false;
  cfg.wifiChannel     = WIFI_CHANNEL;
  cfg.txFramed        = TX_FRAMED;
  cfg.rxAcceptLegacy  = RX_ACCEPT_LEGACY;

  if (!Transport::init(cfg, onRx)) {
    DBG_PRINTLN("[TREX_CTRL] Transport init FAILED");
    while (1) {
      delay(1000);
    }
  }

  sendHello();
  printHelp();  // show help once at boot
}

void loop() {
  // Apply RADIO_CFG only when it comes from the server
  if (gRadioCfgPending) {
    gRadioCfgPending = false;
    applyRadioCfgAndReboot(gRadioCfgMsg);
    return;
  }

  Transport::loop();

  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      handleSerialLine(line);
      line = "";
    } else {
      if (line.length() < 64) line += c;
    }
  }

  pmsTick();

  uint32_t now = millis();
  if (now - gLastPrintMs >= 200) {
    printPeriodicStatus();
    gLastPrintMs = now;
  }
}