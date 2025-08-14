/*
  TREX – T-Rex Server (Feather S3)
  --------------------------------
  Roles:
    • Authoritative game logic & timers (RED/GREEN)
    • PIR input (1–4 sensors, active-LOW, debounced)
    • Serial control of MedeaWiz Sprite 4K (non-blocking cues)
    • Handles loot holds & drops; maintains team score and station inventories
    • Broadcasts STATE_TICK (~5 Hz), SCORE_UPDATE, STATION_UPDATE, GAME_OVER

  Wiring (defaults match your test):
    • PIR_1: GPIO5  (active LOW with INPUT_PULLUP)
      (Add up to 4 PIRs: set pins in PIN_PIR[]; use -1 to disable)
    • Sprite 4K serial: TX=GPIO43 -> Sprite RX, RX=GPIO44 (optional), GND ↔ GND
*/

#include <Arduino.h>
#include <TrexProtocol.h>
#include <TrexTransport.h>
#include <esp_random.h>

/* ── Radio / identity ─────────────────────────────────────── */
constexpr uint8_t  WIFI_CHANNEL = 6;
constexpr uint8_t  STATION_ID   = 0;   // server is always 0

/* ── RED/GREEN cadence ────────────────────────────────────── */
constexpr uint32_t GREEN_MS = 6000;    // tweak to taste
constexpr uint32_t RED_MS   = 4000;

/* ── PIRs (active-LOW). Set to -1 to disable an input. ────── */
int8_t PIN_PIR[4]  = { 5, -1, -1, -1 };
constexpr uint32_t PIR_DEBOUNCE_MS = 60;

/* ── Sprite 4K serial (non-blocking cues) ─────────────────── */
constexpr uint32_t SPRITE_BAUD = 9600;
// Clip map (change to match your SD layout)
constexpr uint8_t CLIP_NOT_LOOKING = 1;   // GREEN loop
constexpr uint8_t CLIP_LOOKING     = 2;   // RED loop
constexpr uint8_t CLIP_GAME_OVER   = 3;   // one-shot

/* ── Team & station config ────────────────────────────────── */
constexpr uint8_t  MAX_PLAYERS   = 24;
constexpr uint8_t  MAX_HOLDS     = 8;     // concurrent holds
constexpr uint8_t  MAX_CARRY     = 8;     // per player carry cap
constexpr uint32_t LOOT_RATE_MS  = 1000;  // +1/sec while GREEN

// Loot station IDs are 1..5
uint16_t stationCapacity[7]  = {0, 56,100,100,100,100, 0};  // index 0,6 unused
uint16_t stationInventory[7] = {0, 56,100,100,100,100, 0};

/* ── Game state ───────────────────────────────────────────── */
enum class Phase : uint8_t { PREGAME=0, PLAYING=1, END=2 };
Phase       g_phase      = Phase::PLAYING;
LightState  g_lightState = LightState::GREEN;
uint32_t    g_nextSwitch = 0;
uint16_t    g_seq        = 1;

struct PlayerRec {
  TrexUid  uid{};
  bool     used=false;
  uint8_t  carried=0;
  uint32_t banked=0;
};

struct HoldRec {
  bool     active=false;
  uint32_t holdId=0;
  uint8_t  stationId=0;
  uint8_t  playerIdx=255;
  uint32_t nextTickAt=0;
};

PlayerRec players[MAX_PLAYERS];
HoldRec   holds[MAX_HOLDS];
uint32_t  teamScore = 0;

/* ── PIR debounce state ───────────────────────────────────── */
struct PirRec {
  int8_t   pin=-1;
  bool     state=false;
  bool     last=false;
  uint32_t lastChange=0;
} g_pir[4];

/* ── Sprite control (fire-and-forget) ─────────────────────── */
void spritePlay(uint8_t clip) {
  Serial.printf("[TREX] Sprite -> play clip %u\n", clip);
  Serial1.write(clip);  // Sprite treats single-byte clip numbers
}

/* ── Transport helpers ────────────────────────────────────── */
void packHeader(uint8_t type, uint16_t payLen, uint8_t* buf, uint16_t seqOverride=0) {
  auto* h=(MsgHeader*)buf;
  h->version=TREX_PROTO_VERSION;
  h->type=type;
  h->srcStationId=STATION_ID;
  h->flags=0;
  h->payloadLen=payLen;
  h->seq = seqOverride ? seqOverride : g_seq++;
}

void sendStateTick(uint32_t msLeft) {
  uint8_t buf[sizeof(MsgHeader)+sizeof(StateTickPayload)];
  packHeader((uint8_t)MsgType::STATE_TICK, sizeof(StateTickPayload), buf);
  auto* p=(StateTickPayload*)(buf+sizeof(MsgHeader));
  p->state = (uint8_t)g_lightState;
  p->msLeft = msLeft;
  Transport::broadcast(buf,sizeof(buf));
}

void bcastGameOver(uint8_t reason /*GameOverReason*/) {
  if (g_phase == Phase::END) return; // single-shot
  g_phase = Phase::END;

  uint8_t buf[sizeof(MsgHeader)+sizeof(GameOverPayload)];
  packHeader((uint8_t)MsgType::GAME_OVER, sizeof(GameOverPayload), buf);
  ((GameOverPayload*)(buf+sizeof(MsgHeader)))->reason = reason;
  Transport::broadcast(buf,sizeof(buf));

  // stop all holds
  for (auto &h : holds) h.active = false;

  // media cue
  spritePlay(CLIP_GAME_OVER);

  Serial.println("[TREX] GAME OVER!");
}

void bcastScore() {
  uint8_t buf[sizeof(MsgHeader)+sizeof(ScoreUpdatePayload)];
  packHeader((uint8_t)MsgType::SCORE_UPDATE, sizeof(ScoreUpdatePayload), buf);
  ((ScoreUpdatePayload*)(buf+sizeof(MsgHeader)))->teamScore = teamScore;
  Transport::broadcast(buf,sizeof(buf));
}

void bcastStation(uint8_t stationId) {
  uint8_t buf[sizeof(MsgHeader)+sizeof(StationUpdatePayload)];
  packHeader((uint8_t)MsgType::STATION_UPDATE, sizeof(StationUpdatePayload), buf);
  auto* p=(StationUpdatePayload*)(buf+sizeof(MsgHeader));
  p->stationId = stationId;
  p->inventory = stationInventory[stationId];
  p->capacity  = stationCapacity[stationId];
  Transport::broadcast(buf,sizeof(buf));
}

void sendDropResult(uint16_t dropped) {
  uint8_t buf[sizeof(MsgHeader)+sizeof(DropResultPayload)];
  packHeader((uint8_t)MsgType::DROP_RESULT, sizeof(DropResultPayload), buf);
  auto* p=(DropResultPayload*)(buf+sizeof(MsgHeader));
  p->dropped = dropped;
  p->teamScore = teamScore;
  Transport::broadcast(buf,sizeof(buf));
}

/* ── Player / Hold helpers ────────────────────────────────── */
bool uidEq(const TrexUid& a, const TrexUid& b) {
  if (a.len != b.len) return false;
  for (uint8_t i=0;i<a.len;i++) if (a.bytes[i]!=b.bytes[i]) return false;
  return true;
}
int findPlayer(const TrexUid& u) {
  for (int i=0;i<MAX_PLAYERS;i++) if (players[i].used && uidEq(players[i].uid,u)) return i;
  return -1;
}
int ensurePlayer(const TrexUid& u) {
  int idx = findPlayer(u);
  if (idx>=0) return idx;
  for (int i=0;i<MAX_PLAYERS;i++) if (!players[i].used) {
    players[i].used=true; players[i].uid=u; players[i].carried=0; players[i].banked=0;
    return i;
  }
  return -1;
}
int findHoldById(uint32_t hid) {
  for (int i=0;i<MAX_HOLDS;i++) if (holds[i].active && holds[i].holdId==hid) return i;
  return -1;
}
int allocHold() {
  for (int i=0;i<MAX_HOLDS;i++) if (!holds[i].active) return i;
  return -1;
}

/* ── Game lifecycle ───────────────────────────────────────── */
void enterGreen() {
  g_lightState = LightState::GREEN;
  g_nextSwitch = millis() + GREEN_MS;
  spritePlay(CLIP_NOT_LOOKING);
  Serial.println("[TREX] -> GREEN");
}
void enterRed() {
  g_lightState = LightState::RED;
  g_nextSwitch = millis() + RED_MS;
  spritePlay(CLIP_LOOKING);
  Serial.println("[TREX] -> RED");

  // If anyone is in the middle of collecting, that's illegal in RED
  for (auto &h : holds) if (h.active) {
    bcastGameOver(/*RED_LOOT*/1);
    break;
  }
}
void startNewGame() {
  Serial.println("[TREX] New game starting...");
  g_phase = Phase::PLAYING;
  teamScore = 0;
  for (int i=0;i<MAX_PLAYERS;i++) { players[i].used=false; players[i].carried=0; players[i].banked=0; }
  for (int i=0;i<MAX_HOLDS;i++) holds[i].active=false;
  for (uint8_t sid=1; sid<=5; ++sid) { stationInventory[sid]=stationCapacity[sid]; bcastStation(sid); }
  bcastScore();
  enterGreen();
}

/* ── RX handler (stations → server) ───────────────────────── */
void onRx(const uint8_t* data, uint16_t len) {
  if (len < sizeof(MsgHeader)) return;
  auto* h = (const MsgHeader*)data;
  if (h->version != TREX_PROTO_VERSION) return;

  switch ((MsgType)h->type) {
    case MsgType::HELLO: {
      Serial.printf("[TREX] HELLO from station %u\n", h->srcStationId);
      break;
    }

    case MsgType::LOOT_HOLD_START: {
      if (h->payloadLen != sizeof(LootHoldStartPayload)) break;
      auto* p = (const LootHoldStartPayload*)(data+sizeof(MsgHeader));
      // Rule: any loot start during RED => GAME OVER
      if (g_phase == Phase::PLAYING && g_lightState == LightState::RED) {
        bcastGameOver(/*RED_LOOT*/1);
        break;
      }
      if (p->stationId < 1 || p->stationId > 5) break;

      int pi = ensurePlayer(p->uid);
      if (pi < 0) {
        // deny (table full)
        uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
        packHeader((uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq /*mirror*/);
        auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
        a->holdId=p->holdId; a->accepted=0; a->rateHz=1; a->maxCarry=MAX_CARRY;
        a->carried=0; a->inventory=stationInventory[p->stationId]; a->capacity=stationCapacity[p->stationId]; a->denyReason=5; // DENIED
        Transport::broadcast(buf,sizeof(buf));
        break;
      }

      // capacity checks
      if (players[pi].carried >= MAX_CARRY) {
        uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
        packHeader((uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
        auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
        a->holdId=p->holdId; a->accepted=0; a->rateHz=1; a->maxCarry=MAX_CARRY;
        a->carried=players[pi].carried; a->inventory=stationInventory[p->stationId]; a->capacity=stationCapacity[p->stationId]; a->denyReason=0; // FULL
        Transport::broadcast(buf,sizeof(buf));
        break;
      }
      if (stationInventory[p->stationId] == 0) {
        uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
        packHeader((uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
        auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
        a->holdId=p->holdId; a->accepted=0; a->rateHz=1; a->maxCarry=MAX_CARRY;
        a->carried=players[pi].carried; a->inventory=0; a->capacity=stationCapacity[p->stationId]; a->denyReason=1; // EMPTY
        Transport::broadcast(buf,sizeof(buf));
        break;
      }

      int hi = allocHold();
      if (hi < 0) {
        uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
        packHeader((uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
        auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
        a->holdId=p->holdId; a->accepted=0; a->rateHz=1; a->maxCarry=MAX_CARRY;
        a->carried=players[pi].carried; a->inventory=stationInventory[p->stationId]; a->capacity=stationCapacity[p->stationId]; a->denyReason=5; // DENIED
        Transport::broadcast(buf,sizeof(buf));
        break;
      }

      holds[hi].active=true; holds[hi].holdId=p->holdId; holds[hi].stationId=p->stationId;
      holds[hi].playerIdx=pi; holds[hi].nextTickAt = millis() + LOOT_RATE_MS;

      // ACK accept (mirror seq so client can dedupe)
      uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
      packHeader((uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
      auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
      a->holdId=p->holdId; a->accepted=1; a->rateHz=1; a->maxCarry=MAX_CARRY;
      a->carried=players[pi].carried; a->inventory=stationInventory[p->stationId]; a->capacity=stationCapacity[p->stationId]; a->denyReason=0;
      Transport::broadcast(buf,sizeof(buf));
      break;
    }

    case MsgType::LOOT_HOLD_STOP: {
      if (h->payloadLen != sizeof(LootHoldStopPayload)) break;
      auto* p = (const LootHoldStopPayload*)(data+sizeof(MsgHeader));
      int hi = findHoldById(p->holdId);
      if (hi>=0) {
        holds[hi].active=false;
        // Notify end (REMOVED)
        uint8_t buf[sizeof(MsgHeader)+sizeof(HoldEndPayload)];
        packHeader((uint8_t)MsgType::HOLD_END, sizeof(HoldEndPayload), buf);
        auto* e=(HoldEndPayload*)(buf+sizeof(MsgHeader));
        e->holdId=p->holdId; e->reason=2; // REMOVED
        Transport::broadcast(buf,sizeof(buf));
      }
      break;
    }

    case MsgType::DROP_REQUEST: {
      if (h->payloadLen != sizeof(DropRequestPayload)) break;
      auto* p = (const DropRequestPayload*)(data+sizeof(MsgHeader));
      int pi = ensurePlayer(p->uid);
      if (pi < 0) break;
      uint16_t dropped = players[pi].carried;
      players[pi].carried = 0;
      players[pi].banked += dropped;
      teamScore += dropped;
      sendDropResult(dropped);
      bcastScore();
      break;
    }

    default: break;
  }
}

/* ── Setup ────────────────────────────────────────────────── */
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n[TREX] Server boot");

  // PIRs
  for (int i=0;i<4;i++) {
    g_pir[i].pin = PIN_PIR[i];
    if (g_pir[i].pin >= 0) {
      pinMode(g_pir[i].pin, INPUT_PULLUP);
      bool initState = (digitalRead(g_pir[i].pin) == LOW); // active-LOW
      g_pir[i].state = g_pir[i].last = initState;
      g_pir[i].lastChange = millis();
      Serial.printf("[TREX] PIR%d on GPIO%d init=%s\n", i, g_pir[i].pin, initState?"TRIG":"IDLE");
    }
  }

  // Sprite serial (TX=43 -> Sprite RX, RX=44 optional)
  Serial1.begin(SPRITE_BAUD, SERIAL_8N1, /*RX*/44, /*TX*/43);
  delay(20);

  // Transport
  TransportConfig cfg{ /*maintenanceMode=*/false, /*wifiChannel=*/WIFI_CHANNEL };
  if (!Transport::init(cfg, onRx)) {
    Serial.println("[TREX] Transport init FAILED");
    while (1) delay(1000);
  }

  // Start game
  startNewGame();
}

/* ── Loop ─────────────────────────────────────────────────── */
void loop() {
  Transport::loop();

  uint32_t now = millis();

  // 1) Broadcast STATE_TICK at ~5 Hz
  static uint32_t lastTick = 0;
  if (now - lastTick >= 200) {
    uint32_t msLeft = (g_nextSwitch > now) ? (g_nextSwitch - now) : 0;
    sendStateTick(msLeft);
    lastTick = now;
  }

  // 2) Cadence flip
  if (g_phase == Phase::PLAYING && now >= g_nextSwitch) {
    (g_lightState == LightState::GREEN) ? enterRed() : enterGreen();
  }

  // 3) PIR monitoring (any sensor)
  if (g_phase == Phase::PLAYING && g_lightState == LightState::RED) {
    bool anyTrig = false;
    for (int i=0;i<4;i++) if (g_pir[i].pin >= 0) {
      bool raw = (digitalRead(g_pir[i].pin) == LOW);
      if (raw != g_pir[i].last && (now - g_pir[i].lastChange) > PIR_DEBOUNCE_MS) {
        g_pir[i].last = raw; g_pir[i].lastChange = now;
        g_pir[i].state = raw;
        if (raw) { anyTrig = true; } // rising to TRIGGERED
      }
    }
    if (anyTrig) {
      bcastGameOver(/*RED_PIR*/0);
    }
  }

  // 4) Active loot holds (server-driven tick @ 1Hz while GREEN)
  if (g_phase == Phase::PLAYING && g_lightState == LightState::GREEN) {
    for (int i=0;i<MAX_HOLDS;i++) if (holds[i].active) {
      auto &h = holds[i];
      auto &pl = players[h.playerIdx];
      uint8_t sid = h.stationId;

      if (now >= h.nextTickAt) {
        if (pl.carried >= MAX_CARRY) {
          h.active=false;
          // HOLD_END: FULL
          uint8_t buf[sizeof(MsgHeader)+sizeof(HoldEndPayload)];
          packHeader((uint8_t)MsgType::HOLD_END, sizeof(HoldEndPayload), buf);
          auto* e=(HoldEndPayload*)(buf+sizeof(MsgHeader));
          e->holdId=h.holdId; e->reason=0; // FULL
          Transport::broadcast(buf,sizeof(buf));
          continue;
        }
        if (stationInventory[sid] == 0) {
          h.active=false;
          // HOLD_END: EMPTY
          uint8_t buf[sizeof(MsgHeader)+sizeof(HoldEndPayload)];
          packHeader((uint8_t)MsgType::HOLD_END, sizeof(HoldEndPayload), buf);
          auto* e=(HoldEndPayload*)(buf+sizeof(MsgHeader));
          e->holdId=h.holdId; e->reason=1; // EMPTY
          Transport::broadcast(buf,sizeof(buf));
          continue;
        }

        // Apply +1/-1 for this tick
        pl.carried += 1;
        stationInventory[sid] -= 1;

        // Send LOOT_TICK + STATION_UPDATE
        {
          uint8_t buf[sizeof(MsgHeader)+sizeof(LootTickPayload)];
          packHeader((uint8_t)MsgType::LOOT_TICK, sizeof(LootTickPayload), buf);
          auto* t=(LootTickPayload*)(buf+sizeof(MsgHeader));
          t->holdId=h.holdId; t->carried=pl.carried; t->inventory=stationInventory[sid];
          Transport::broadcast(buf,sizeof(buf));
        }
        bcastStation(sid);

        h.nextTickAt += LOOT_RATE_MS;
      }
    }
  }

  // 5) Serial console commands (quick testing)
  while (Serial.available()) {
    int c = Serial.read();
    if (c=='n' || c=='N') startNewGame();
    if (c=='g' || c=='G') enterGreen();
    if (c=='r' || c=='R') enterRed();
    if (c=='x' || c=='X') bcastGameOver(/*MANUAL*/2);
  }
}
