#include <TrexTransport.h>
#include <esp_random.h>
#include <TrexProtocol.h>
#include "Net.h"
#include "Media.h"
#include "OtaCampaign.h"
#include "GameAudio.h"
#include "Bonus.h"
#include "ServerMini.h"

// From main server sketch
extern void startNewGame(Game& g);

// --- Maintenance / control request flags (CONTROL -> server) ---
static bool sEnterMaintRequested   = false;
static bool sControlStartRequested = false;
static bool sControlStopRequested  = false;
static bool sLootOtaRequested      = false;

bool netConsumeEnterMaintRequest() {
  bool v = sEnterMaintRequested;
  sEnterMaintRequested = false;
  return v;
}

bool netConsumeControlStartRequest() {
  bool v = sControlStartRequested;
  sControlStartRequested = false;
  return v;
}

bool netConsumeControlStopRequest() {
  bool v = sControlStopRequested;
  sControlStopRequested = false;
  return v;
}

bool netConsumeLootOtaRequest() {
  bool v = sLootOtaRequested;
  sLootOtaRequested = false;
  return v;
}

// Generic raw broadcast used by OTA
void netBroadcastRaw(const uint8_t* data, uint16_t len) {
  Transport::broadcast(data, len);
}

static void packHeader(Game& g, uint8_t type, uint16_t payLen, uint8_t* buf, uint16_t seqOverride=0) {
  auto* h=(MsgHeader*)buf;
  h->version=TREX_PROTO_VERSION;
  h->type=type;
  h->srcStationId=STATION_ID;
  h->flags=0;
  h->payloadLen=payLen;
  h->seq = seqOverride ? seqOverride : g.seq++;
}

void sendStateTick(const Game& g, uint32_t msLeft) {
  uint8_t buf[sizeof(MsgHeader)+sizeof(StateTickPayload)];
  auto* p=(StateTickPayload*)(buf+sizeof(MsgHeader));
  packHeader(const_cast<Game&>(g), (uint8_t)MsgType::STATE_TICK, sizeof(StateTickPayload), buf);
  p->state = (uint8_t)g.light;
  p->msLeft = msLeft;
  Transport::broadcast(buf,sizeof(buf));
}

void bcastGameStart(Game& g) {
  uint8_t buf[sizeof(MsgHeader)];
  packHeader(g, (uint8_t)MsgType::GAME_START, 0, buf);
  bool ok = Transport::broadcast(buf, sizeof(buf));
  Serial.printf("[TREX] GAME_START broadcast %s\n", ok ? "OK" : "FAILED");
}

void bcastGameOver(Game& g, uint8_t reason, uint8_t blameSid /*=GAMEOVER_BLAME_ALL*/) {
  if (g.phase == Phase::END) return; // single-shot
  g.phase = Phase::END;

  // Kill any intermission/bonus and notify clients so rainbow stops
  g.bonusIntermission = false;
  g.bonusActiveMask   = 0;
  for (uint8_t sid = 1; sid <= MAX_STATIONS; ++sid) g.bonusEndsAt[sid] = 0;
  bcastBonusUpdate(g);  // mask=0

  // Stop any looping SFX before we play the lose sting
  gameAudioStop();

  // GAME_OVER payload
  uint8_t buf[sizeof(MsgHeader) + sizeof(GameOverPayload)];
  packHeader(g, (uint8_t)MsgType::GAME_OVER, sizeof(GameOverPayload), buf);
  auto *p = (GameOverPayload*)(buf + sizeof(MsgHeader));
  p->reason   = reason;
  p->blameSid = blameSid;
  Transport::broadcast(buf,sizeof(buf));

  // Final STATE_TICK with 0 to freeze any wall timers
  sendStateTick(g, 0);
  // keep scheduler from immediately sending more ticks
  g.lastTickSentMs = millis();

  // Clean up holds and media
  for (auto &h : g.holds) h.active = false;
  gameAudioPlayOnce(TRK_TREX_LOSE);
  spritePlay(CLIP_GAME_OVER);
  Serial.printf("[TREX] GAME OVER! reason=%u blameSid=%u\n", reason, blameSid);
}

void bcastScore(Game& g) {
  uint8_t buf[sizeof(MsgHeader)+sizeof(ScoreUpdatePayload)];
  packHeader(g, (uint8_t)MsgType::SCORE_UPDATE, sizeof(ScoreUpdatePayload), buf);
  ((ScoreUpdatePayload*)(buf+sizeof(MsgHeader)))->teamScore = g.teamScore;
  Transport::broadcast(buf,sizeof(buf));
}

void bcastStation(Game& g, uint8_t stationId) {
  uint8_t buf[sizeof(MsgHeader)+sizeof(StationUpdatePayload)];
  packHeader(g, (uint8_t)MsgType::STATION_UPDATE, sizeof(StationUpdatePayload), buf);
  auto* p=(StationUpdatePayload*)(buf+sizeof(MsgHeader));
  p->stationId = stationId;
  p->inventory = g.stationInventory[stationId];
  p->capacity  = g.stationCapacity[stationId];
  Transport::broadcast(buf,sizeof(buf));
}

void bcastRoundStatus(Game& g) {
  uint8_t buf[sizeof(MsgHeader)+sizeof(RoundStatusPayload)];
  packHeader(g, (uint8_t)MsgType::ROUND_STATUS, sizeof(RoundStatusPayload), buf);
  auto* p = (RoundStatusPayload*)(buf + sizeof(MsgHeader));
  p->roundIndex     = g.roundIndex;
  p->reserved       = 0;
  p->_pad           = 0;
  p->roundStartScore= g.roundStartScore;
  p->roundGoalAbs   = g.roundGoal;
  const uint32_t now = millis();
  p->msLeftRound    = (g.roundEndAt > now) ? (g.roundEndAt - now) : 0;
  Transport::broadcast(buf, sizeof(buf));
}

void bcastBonusUpdate(Game& g) {
  uint8_t buf[sizeof(MsgHeader) + sizeof(BonusUpdatePayload)];
  packHeader(g, (uint8_t)MsgType::BONUS_UPDATE, sizeof(BonusUpdatePayload), buf);
  auto* p = (BonusUpdatePayload*)(buf + sizeof(MsgHeader));
  p->mask = g.bonusActiveMask;
  Transport::broadcast(buf, sizeof(buf));
}

// --- Game status broadcast for Control station ---
void bcastGameStatus(const Game& g) {
  uint8_t buf[sizeof(MsgHeader) + sizeof(GameStatusPayload)];
  Game& ng = const_cast<Game&>(g);
  packHeader(ng, (uint8_t)MsgType::GAME_STATUS, sizeof(GameStatusPayload), buf);
  auto* p = (GameStatusPayload*)(buf + sizeof(MsgHeader));

  const uint32_t now = millis();

  uint32_t msLeftGame = 0;
  if (g.gameEndAt > 0 && g.gameEndAt > now) {
    msLeftGame = g.gameEndAt - now;
  }

  uint32_t msLeftRound = 0;
  if (g.phase == Phase::PLAYING) {
    if      (g.bonusIntermission)  msLeftRound = (g.bonusInterEnd  > now) ? (g.bonusInterEnd  - now) : 0;
    else if (g.bonusIntermission2) msLeftRound = (g.bonus2End      > now) ? (g.bonus2End      - now) : 0;
    else                           msLeftRound = (g.roundEndAt     > now) ? (g.roundEndAt     - now) : 0;
  }

  p->teamScore    = g.teamScore;
  p->msLeftGame   = msLeftGame;
  p->msLeftRound  = msLeftRound;
  p->roundIndex   = g.roundIndex;
  p->phase        = (uint8_t)g.phase;
  p->lightState   = (uint8_t)g.light;
  p->_pad         = 0;

  Transport::broadcast(buf, sizeof(buf));
}

void bcastMgStart(Game& g, const Game::MgConfig& c) {
  uint8_t buf[sizeof(MsgHeader) + sizeof(MgStartPayload)];
  packHeader(g, (uint8_t)MsgType::MG_START, sizeof(MgStartPayload), buf);
  auto* p = (MgStartPayload*)(buf + sizeof(MsgHeader));
  p->seed       = c.seed;
  p->timerMs    = c.timerMs;
  p->speedMinMs = c.speedMinMs;
  p->speedMaxMs = c.speedMaxMs;
  p->segMin     = c.segMin;
  p->segMax     = c.segMax;
  Transport::broadcast(buf, sizeof buf);
}

void bcastMgStop(Game& g) {
  uint8_t buf[sizeof(MsgHeader)];
  packHeader(g, (uint8_t)MsgType::MG_STOP, 0, buf);
  Transport::broadcast(buf, sizeof buf);
}

void sendDropResult(Game& g, uint16_t dropped, uint8_t readerIndex /*=DROP_READER_UNKNOWN*/) {
  uint8_t buf[sizeof(MsgHeader) + sizeof(DropResultPayload)];
  packHeader(g, (uint8_t)MsgType::DROP_RESULT, sizeof(DropResultPayload), buf);

  auto* p = (DropResultPayload*)(buf + sizeof(MsgHeader));
  p->dropped     = dropped;
  p->teamScore   = g.teamScore;
  p->readerIndex = readerIndex;

  Transport::broadcast(buf, sizeof(buf));
}

void sendHoldEnd(Game& g, uint32_t holdId, uint8_t reason) {
  uint8_t buf[sizeof(MsgHeader)+sizeof(HoldEndPayload)];
  packHeader(g, (uint8_t)MsgType::HOLD_END, sizeof(HoldEndPayload), buf);
  auto* e=(HoldEndPayload*)(buf+sizeof(MsgHeader));
  e->holdId=holdId; e->reason=reason;
  Transport::broadcast(buf,sizeof(buf));
}

void sendLootTick(Game& g, uint32_t holdId, uint8_t carried, uint16_t stationInv) {
  uint8_t buf[sizeof(MsgHeader)+sizeof(LootTickPayload)];
  packHeader(g, (uint8_t)MsgType::LOOT_TICK, sizeof(LootTickPayload), buf);
  auto* t=(LootTickPayload*)(buf+sizeof(MsgHeader));
  t->holdId=holdId; t->carried=carried; t->inventory=stationInv;
  Transport::broadcast(buf,sizeof(buf));
}

/* ── RX handler (stations → server) ───────────────────────── */
void onRx(const uint8_t* data, uint16_t len) {
  if (len < sizeof(MsgHeader)) return;
  auto* h = (const MsgHeader*)data;
  if (h->version != TREX_PROTO_VERSION) {
    Serial.printf("[WARN] Proto mismatch on RX: got=%u exp=%u (type=%u)\n",
                  h->version, (unsigned)TREX_PROTO_VERSION, h->type);
    return;
  }

  // Debug: log *every* incoming message type
  Serial.printf("[NET] RX type=%u len=%u from=%u\n",
                (unsigned)h->type,
                (unsigned)h->payloadLen,
                (unsigned)h->srcStationId);

  if (OtaCampaign::handle(data, len)) return;

  switch ((MsgType)h->type) {
    case MsgType::HELLO: {
      Serial.printf("[TREX] HELLO from station %u\n", h->srcStationId);
      break;
    }

    // CONTROL_CMD from Control station (START/STOP/MAINT/LOOT)
    case MsgType::CONTROL_CMD: {
      if (h->payloadLen != sizeof(ControlCmdPayload)) {
        Serial.printf("[TREX] CONTROL_CMD bad len=%u (expected %u)\n",
                      (unsigned)h->payloadLen,
                      (unsigned)sizeof(ControlCmdPayload));
        break;
      }
      auto* p = (const ControlCmdPayload*)(data + sizeof(MsgHeader));

      extern Game g;
      Game& G = g;

      Serial.printf("[TREX] CONTROL_CMD op=%u targetType=%u targetId=%u from station %u\n",
                    (unsigned)p->op,
                    (unsigned)p->targetType,
                    (unsigned)p->targetId,
                    (unsigned)h->srcStationId);

      // For START/STOP/ENTER_MAINT we only act if the command targets the TREX server.
      const uint8_t myType = (uint8_t)StationType::TREX;
      const uint8_t myId   = STATION_ID; // 0 for server

      auto matchesTrex = [&](void) -> bool {
        // 255 = wildcard for CONTROL_CMD (all types / all ids)
        bool typeMatch = (p->targetType == myType || p->targetType == 255);
        bool idMatch   = (p->targetId   == myId   || p->targetId   == 255);
        return typeMatch && idMatch;
      };

      switch ((ControlOp)p->op) {
        case ControlOp::START: {
          if (matchesTrex()) {
            sControlStartRequested = true;
          }
          break;
        }

        case ControlOp::STOP: {
          if (matchesTrex()) {
            sControlStopRequested = true;
          }
          break;
        }

        case ControlOp::ENTER_MAINT: {
          if (matchesTrex()) {
            sEnterMaintRequested = true;
          }
          break;
        }

        case ControlOp::LOOT_OTA: {
          // Always orchestrated by the server; targetId says which Loot station(s) to OTA.
          // 0 => all loot stations (OtaCampaign treats 0 as wildcard for ConfigUpdate targetId)
          // N => only loot station with STATION_ID == N
          OtaCampaign::setLootTargetId(p->targetId);
          sLootOtaRequested = true;
          break;
        }

        default:
          Serial.printf("[TREX] CONTROL_CMD unknown op=%u\n", (unsigned)p->op);
          break;
      }
      break;
    }

    case MsgType::LOOT_HOLD_START: {
      if (h->payloadLen != sizeof(LootHoldStartPayload)) break;
      auto* p = (const LootHoldStartPayload*)(data + sizeof(MsgHeader));

      extern Game g; Game& G = g;
      const uint32_t now = millis();

      // Compute truthful rateHz from lootRateMs (used in all ACKs)
      uint8_t rateHz = 1;
      if (G.lootRateMs > 0) {
        uint32_t hz = 1000U / G.lootRateMs;
        if (hz < 1)   hz = 1;
        if (hz > 255) hz = 255;
        rateHz = (uint8_t)hz;
      }

      // Validate basic conditions (phase/station id)
      if (G.phase != Phase::PLAYING || p->stationId < 1 || p->stationId > 5) {
        uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
        packHeader(G, (uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
        auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
        a->holdId=p->holdId; a->accepted=0; a->rateHz=rateHz; a->maxCarry=G.maxCarry;
        a->carried=0;
        a->inventory=(p->stationId>=1 && p->stationId<=5) ? G.stationInventory[p->stationId] : 0;
        a->capacity =(p->stationId>=1 && p->stationId<=5) ? G.stationCapacity[p->stationId]  : 0;
        a->denyReason=5; // DENIED (bad state or bad station)
        Transport::broadcast(buf,sizeof(buf));
        break;
      }

      // --- RED handling only (YELLOW is allowed like GREEN) ---
      if (G.light == LightState::RED) {
        uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
        packHeader(G, (uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
        auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
        a->holdId=p->holdId; a->accepted=0; a->rateHz=rateHz; a->maxCarry=G.maxCarry;
        a->carried=0;
        a->inventory= G.stationInventory[p->stationId];
        a->capacity = G.stationCapacity[p->stationId];
        if (now - G.lastFlipMs <= G.edgeGraceMs) a->denyReason=6; else a->denyReason=2;
        Transport::broadcast(buf,sizeof(buf));
        break;
      }

      // Ensure player record
      int pi = ensurePlayer(G, p->uid);
      if (pi < 0) {
        uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
        packHeader(G, (uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
        auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
        a->holdId=p->holdId; a->accepted=0; a->rateHz=rateHz; a->maxCarry=G.maxCarry;
        a->carried=0;
        a->inventory=G.stationInventory[p->stationId];
        a->capacity =G.stationCapacity[p->stationId];
        a->denyReason=5; // DENIED (no player)
        Transport::broadcast(buf,sizeof(buf));
        break;
      }

      // Full carry?
      if (G.players[pi].carried >= G.maxCarry) {
        uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
        packHeader(G, (uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
        auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
        a->holdId=p->holdId; a->accepted=0; a->rateHz=rateHz; a->maxCarry=G.maxCarry;
        a->carried=G.players[pi].carried;
        a->inventory=G.stationInventory[p->stationId];
        a->capacity =G.stationCapacity[p->stationId];
        a->denyReason=0; // FULL
        Transport::broadcast(buf,sizeof(buf));
        break;
      }

      // Station empty?
      if (G.stationInventory[p->stationId] == 0) {
        uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
        packHeader(G, (uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
        auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
        a->holdId=p->holdId; a->accepted=0; a->rateHz=rateHz; a->maxCarry=G.maxCarry;
        a->carried=G.players[pi].carried;
        a->inventory=0;
        a->capacity =G.stationCapacity[p->stationId];
        a->denyReason=1; // EMPTY
        Transport::broadcast(buf,sizeof(buf));
        break;
      }

      // Allocate hold slot
      int hi = allocHold(G);
      if (hi < 0) {
        uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
        packHeader(G, (uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
        auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
        a->holdId=p->holdId; a->accepted=0; a->rateHz=rateHz; a->maxCarry=G.maxCarry;
        a->carried=G.players[pi].carried;
        a->inventory=G.stationInventory[p->stationId];
        a->capacity =G.stationCapacity[p->stationId];
        a->denyReason=5; // DENIED (no slots)
        Transport::broadcast(buf,sizeof(buf));
        break;
      }

      // Accept hold
      G.holds[hi].active     = true;
      G.holds[hi].holdId     = p->holdId;
      G.holds[hi].stationId  = p->stationId;
      G.holds[hi].playerIdx  = pi;
      G.holds[hi].nextTickAt = now + (G.lootRateMs ? G.lootRateMs : 250); // safe fallback

      {
        uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
        packHeader(G, (uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
        auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
        a->holdId=p->holdId; a->accepted=1; a->rateHz=rateHz; a->maxCarry=G.maxCarry;
        a->carried=G.players[pi].carried;
        a->inventory=G.stationInventory[p->stationId];
        a->capacity =G.stationCapacity[p->stationId];
        a->denyReason=0;
        Transport::broadcast(buf,sizeof(buf));
      }

      if (applyBonusOnHoldStart(G, G.holds[hi].playerIdx, G.holds[hi].stationId, G.holds[hi].holdId)) {
        G.holds[hi].active = false;
      }
      break;
    }

    case MsgType::LOOT_HOLD_STOP: {
      if (h->payloadLen != sizeof(LootHoldStopPayload)) break;
      auto* p = (const LootHoldStopPayload*)(data+sizeof(MsgHeader));

      extern Game g; Game& G = g;
      int hi = findHoldById(G, p->holdId);
      if (hi>=0) {
        G.holds[hi].active=false;
        sendHoldEnd(G, p->holdId, /*REMOVED*/2);
      }
      break;
    }

    case MsgType::DROP_REQUEST: {
      if (h->payloadLen != sizeof(DropRequestPayload)) break;
      auto* p = (const DropRequestPayload*)(data+sizeof(MsgHeader));

      extern Game g; Game& G = g;
      int pi = ensurePlayer(G, p->uid);
      if (pi < 0) break;

      uint16_t dropped = G.players[pi].carried;
      G.players[pi].carried = 0;
      G.players[pi].banked += dropped;
      G.teamScore += dropped;

      sendDropResult(G, dropped, p->readerIndex);
      bcastScore(G);
      break;
    }

    case MsgType::MG_RESULT: {
      if (h->payloadLen != sizeof(MgResultPayload)) break;
      const auto* p = (const MgResultPayload*)(data + sizeof(MsgHeader));

      extern Game g; Game& G = g;
      if (!G.mgActive) break;
      if (p->stationId < 1 || p->stationId > MAX_STATIONS) break;

      const uint32_t bit = (1u << p->stationId);

      if (!(G.mgTriedMask & bit)) {
        G.mgTriedMask |= bit;
        if (p->success) {
          G.mgSuccessMask |= bit;
          G.teamScore += 10;
          bcastScore(G);
        }
        uint32_t allMask = 0; for (uint8_t sid=1; sid<=MAX_STATIONS; ++sid) allMask |= (1u<<sid);
        if ((G.mgTriedMask & allMask) == allMask && G.mgAllTriedAt == 0) {
          G.mgAllTriedAt = millis();
        }
      }
      break;
    }

    default:
      break;
  }
}
