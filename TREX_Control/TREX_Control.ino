/*
  TREX Control Station (Feather S3)
*/

#include <Arduino.h>
#include <TrexProtocol.h>
#include <TrexTransport.h>
#include <TrexVersion.h>

// Radio identity for this station
constexpr uint8_t WIFI_CHANNEL = 6;   // must match server
constexpr uint8_t STATION_ID   = 6;   // unique id for CONTROL station

// Station IDs for targeting
constexpr uint8_t SERVER_STATION_ID   = 0; // T-Rex server
constexpr uint8_t DROPOFF_STATION_ID  = 6; // your Drop-off station id
// Loot stations are typically 1..5

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

// --- Network RX: update snapshot + emit events -------------------------

void onRx(const uint8_t* data, uint16_t len) {
  if (len < sizeof(MsgHeader)) return;
  auto* h = (const MsgHeader*)data;
  if (h->version != TREX_PROTO_VERSION) return;

  const uint8_t* payload = data + sizeof(MsgHeader);

  switch ((MsgType)h->type) {
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
        Serial.printf("EVENT LIFE_LOST reason=%u blameSid=%u lives=%u/%u\n",
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
      gStatus.teamScore = p->teamScore;
      Serial.printf("EVENT SCORE score=%lu\n", (unsigned long)gStatus.teamScore);
      break;
    }

    case MsgType::GAME_OVER: {
      if (h->payloadLen < sizeof(GameOverPayload)) break;
      auto* p = (const GameOverPayload*)payload;
      (void)p;
      gStatus.phase = 2;  // END
      uint32_t score = gStatus.teamScore;
      Serial.printf("EVENT GAME_OVER reason=%u score=%lu\n",
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
    Serial.println("OK MAINT SERVER");
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
    Serial.println("OK MAINT SERVER");
    return;
  }

  // MAINT SERVER
  if (rest == "SERVER") {
    sendControl(ControlOp::ENTER_MAINT,
                (uint8_t)StationType::TREX,
                0);         // TREX only (server)
    gServerInMaint = true;
    Serial.println("OK MAINT SERVER");
    return;
  }

  // MAINT DROP / MAINT DROPOFF
  if (rest == "DROP" || rest == "DROPOFF") {
    sendControl(ControlOp::ENTER_MAINT,
                (uint8_t)StationType::DROP,
                DROPOFF_STATION_ID);   // just the drop-off station
    Serial.println("OK MAINT DROPOFF");
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
      Serial.println("OK MAINT LOOT ALL");
      return;
    }

    // MAINT LOOT N
    int lootId = lootRest.toInt();
    if (lootId <= 0 || lootId > 255) {
      Serial.println("ERR MAINT LOOT expects 1..255 or ALL");
      Serial.println("     e.g. MAINT LOOT 3   (loot station 3)");
      return;
    }
    sendControl(ControlOp::ENTER_MAINT,
                (uint8_t)StationType::LOOT,
                (uint8_t)lootId);      // specific loot station id
    Serial.print("OK MAINT LOOT ");
    Serial.println(lootId);
    return;
  }

  // MAINT ALL -> targetType=255, targetId=255 (true wildcard for CONTROL_CMD)
  if (rest == "ALL") {
    sendControl(ControlOp::ENTER_MAINT,
                255,   // all types (TREX + LOOT + DROP + CONTROL)
                255);  // all ids
    gServerInMaint = true;  // includes server
    Serial.println("OK MAINT ALL");
    return;
  }

  Serial.println("ERR MAINT usage:");
  Serial.println("  MAINT                  (server)");
  Serial.println("  MAINT SERVER");
  Serial.println("  MAINT DROPOFF");
  Serial.println("  MAINT LOOT             (all loot stations)");
  Serial.println("  MAINT LOOT ALL");
  Serial.println("  MAINT LOOT <N>         (loot station N, 1..5)");
  Serial.println("  MAINT ALL              (server + drop-off + all loot)");
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
    Serial.println("OK LOOT ALL");
    return;
  }

  String rest = cmd.substring(4);
  rest.trim();

  if (rest.length() == 0 || rest == "ALL") {
    sendControl(ControlOp::LOOT_OTA,
                (uint8_t)StationType::TREX,
                0);
    Serial.println("OK LOOT ALL");
    return;
  }

  int lootId = rest.toInt();
  if (lootId <= 0 || lootId > 255) {
    Serial.println("ERR LOOT expects 1..255 or ALL");
    Serial.println("     e.g. LOOT 3        (OTA loot station 3 only)");
    return;
  }

  // Here: targetId = loot station id; server will use this to aim the OTA campaign.
  sendControl(ControlOp::LOOT_OTA,
              (uint8_t)StationType::TREX,   // still routed to server
              (uint8_t)lootId);             // which loot id(s) to update
  Serial.print("OK LOOT ");
  Serial.println(lootId);
}

void printHelp() {
  Serial.println();
  Serial.println("=== T-REX CONTROL HELP ===");
  Serial.println();
  Serial.println("Core game control:");
  Serial.println("  START             - Start a new game on the server");
  Serial.println("  STOP              - End the current game (manual GAME_OVER)");
  Serial.println();
  Serial.println("Maintenance / OTA (enter ArduinoOTA + Telnet, etc.):");
  Serial.println("  MAINT             - Maintenance mode on server (same as MAINT SERVER)");
  Serial.println("  MAINT SERVER      - Server only (T-Rex logic)");
  Serial.println("  MAINT DROPOFF     - Drop-off station only");
  Serial.println("  MAINT LOOT        - All loot stations");
  Serial.println("  MAINT LOOT ALL    - Same as above");
  Serial.println("  MAINT LOOT <N>    - Single loot station (N = 1..5)");
  Serial.println("  MAINT ALL         - Server + Drop-off + all loot stations");
  Serial.println();
  Serial.println("Loot firmware OTA (via server campaign):");
  Serial.println("  LOOT              - OTA ALL loot stations");
  Serial.println("  LOOT ALL          - Same as above");
  Serial.println("  LOOT <N>          - OTA loot station N only (N = 1..5)");
  Serial.println();
  Serial.println("Status / debug:");
  Serial.println("  STATUS            - Print one status line immediately");
  Serial.println("                       (phase, round, score, msGame, msRound, light)");
  Serial.println("  HELP              - Show this help");
  Serial.println();
  Serial.println("Examples:");
  Serial.println("  MAINT SERVER      -> put only the server into OTA/Telnet mode");
  Serial.println("  MAINT DROPOFF     -> OTA/Telnet for Drop-off only");
  Serial.println("  MAINT LOOT 3      -> maintenance for loot station id=3");
  Serial.println("  LOOT 2            -> OTA firmware to loot station id=2 only");
  Serial.println("  LOOT ALL          -> OTA firmware to all loot stations");
  Serial.println("  MAINT ALL         -> maintenance on entire T-Rex network");
  Serial.println();
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
    gServerInMaint = false;
    Serial.println("OK START");
  } else if (cmd == "STOP") {
    sendControl(ControlOp::STOP,
                (uint8_t)StationType::TREX,
                0);
    Serial.println("OK STOP");
  } else if (cmd.startsWith("MAINT")) {
    handleMaintCommand(cmd);
  } else if (cmd.startsWith("LOOT")) {
    handleLootCommand(cmd);
  } else if (cmd == "STATUS") {
    if (gServerInMaint) {
      Serial.printf("STATUS phase=MAINT score=%lu\n",
                    (unsigned long)gStatus.teamScore);
    } else if (!gStatus.hasStatus) {
      Serial.println("STATUS phase=UNKNOWN score=0");
    } else {
      Serial.printf("STATUS phase=%s round=%u score=%lu msGame=%lu msRound=%lu light=%s lives=%u/%u\n",
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
    Serial.print("ERR unknown command: ");
    Serial.println(cmd);
    Serial.println("Type HELP for a list of commands.");
  }
}

// --- Periodic STATUS print --------------------------------------------

void printPeriodicStatus() {
  if (gServerInMaint) return;
  if (!gStatus.hasStatus) return;
  if (gStatus.phase != 1) return; // only while PLAYING

  Serial.printf("STATUS phase=%s round=%u score=%lu msGame=%lu msRound=%lu light=%s lives=%u/%u\n",
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
  Serial.println("\n[TREX_CTRL] Control station boot");

  TransportConfig cfg;
  cfg.maintenanceMode = false;
  cfg.wifiChannel     = WIFI_CHANNEL;

  if (!Transport::init(cfg, onRx)) {
    Serial.println("[TREX_CTRL] Transport init FAILED");
    while (1) {
      delay(1000);
    }
  }

  sendHello();
  printHelp();  // show help once at boot
}

void loop() {
  Transport::loop();

  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      handleCommand(line);
      line = "";
    } else {
      if (line.length() < 64) line += c;
    }
  }

  uint32_t now = millis();
  if (now - gLastPrintMs >= 200) {
    printPeriodicStatus();
    gLastPrintMs = now;
  }
}
