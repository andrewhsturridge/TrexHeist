#include "Net.h"
#include "Media.h"
#include <TrexTransport.h>
#include <esp_random.h>
#include "OtaCampaign.h"

// Generic raw broadcast used by OTA
void netBroadcastRaw(const uint8_t* data, uint16_t len) {
  // Use the same Transport call you already use in your bcast* helpers:
  Transport::broadcast(data, len);        // ← keep this if you have 'broadcast(...)'
  // Transport::sendToAll(data, len);     // ← OR keep this if your Transport uses 'sendToAll(...)'
  // Transport::sendAll(data, len);       // ← OR whatever your project calls it
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

void bcastGameOver(Game& g, uint8_t reason /*GameOverReason*/) {
  if (g.phase == Phase::END) return; // single-shot
  g.phase = Phase::END;

  uint8_t buf[sizeof(MsgHeader)+sizeof(GameOverPayload)];
  packHeader(g, (uint8_t)MsgType::GAME_OVER, sizeof(GameOverPayload), buf);
  ((GameOverPayload*)(buf+sizeof(MsgHeader)))->reason = reason;
  Transport::broadcast(buf,sizeof(buf));

  // stop all holds
  for (auto &h : g.holds) h.active = false;

  spritePlay(CLIP_GAME_OVER);
  Serial.println("[TREX] GAME OVER!");
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
  Serial.printf("[BCAST_STATION] sid=%u inv=%u cap=%u\n",
                stationId,
                g.stationInventory[stationId],
                g.stationCapacity[stationId]);
}

void sendDropResult(Game& g, uint16_t dropped) {
  uint8_t buf[sizeof(MsgHeader)+sizeof(DropResultPayload)];
  packHeader(g, (uint8_t)MsgType::DROP_RESULT, sizeof(DropResultPayload), buf);
  auto* p=(DropResultPayload*)(buf+sizeof(MsgHeader));
  p->dropped = dropped;
  p->teamScore = g.teamScore;
  Transport::broadcast(buf,sizeof(buf));
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

  if (OtaCampaign::handle(data, len)) return;

  switch ((MsgType)h->type) {
    case MsgType::HELLO: {
      Serial.printf("[TREX] HELLO from station %u\n", h->srcStationId);
      break;
    }

  case MsgType::LOOT_HOLD_START: {
    if (h->payloadLen != sizeof(LootHoldStartPayload)) break;
    auto* p = (const LootHoldStartPayload*)(data + sizeof(MsgHeader));

    extern Game g; Game& G = g;

    const uint32_t now = millis();

    // Compute truthful rateHz from lootRateMs (used in all ACKs below)
    uint8_t rateHz = 1;
    if (G.lootRateMs > 0) {
      uint32_t hz = 1000U / G.lootRateMs;
      if (hz < 1)   hz = 1;
      if (hz > 255) hz = 255;
      rateHz = (uint8_t)hz;
    }

    // Validate basic conditions
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

    // RED violation handling with edge grace
    if (G.light == LightState::RED) {
      if (now - G.lastFlipMs <= G.edgeGraceMs) {
        // within grace: deny politely (no game over)
        uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
        packHeader(G, (uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
        auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
        a->holdId=p->holdId; a->accepted=0; a->rateHz=rateHz; a->maxCarry=G.maxCarry;
        a->carried=0;
        a->inventory=G.stationInventory[p->stationId];
        a->capacity =G.stationCapacity[p->stationId];
        a->denyReason=6; // EDGE_GRACE
        Transport::broadcast(buf,sizeof(buf));
        break;
      }
      // outside grace: end game
      bcastGameOver(G, /*RED_LOOT*/1);
      break;
    }

    // Ensure player record
    int pi = ensurePlayer(G, p->uid);
    if (pi < 0) {
      uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
      packHeader(G, (uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
      auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
      a->holdId=p->holdId; a->accepted=0; a->rateHz=rateHz; a->maxCarry=G.maxCarry;
      a->carried=0; a->inventory=G.stationInventory[p->stationId];
      a->capacity=G.stationCapacity[p->stationId]; a->denyReason=5; // DENIED
      Transport::broadcast(buf,sizeof(buf));
      break;
    }

    // Full carry?
    if (G.players[pi].carried >= G.maxCarry) {
      uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
      packHeader(G, (uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
      auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
      a->holdId=p->holdId; a->accepted=0; a->rateHz=rateHz; a->maxCarry=G.maxCarry;
      a->carried=G.players[pi].carried; a->inventory=G.stationInventory[p->stationId];
      a->capacity=G.stationCapacity[p->stationId]; a->denyReason=0; // FULL
      Transport::broadcast(buf,sizeof(buf));
      break;
    }

    // Station empty?
    if (G.stationInventory[p->stationId] == 0) {
      uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
      packHeader(G, (uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
      auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
      a->holdId=p->holdId; a->accepted=0; a->rateHz=rateHz; a->maxCarry=G.maxCarry;
      a->carried=G.players[pi].carried; a->inventory=0;
      a->capacity=G.stationCapacity[p->stationId]; a->denyReason=1; // EMPTY
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
      a->carried=G.players[pi].carried; a->inventory=G.stationInventory[p->stationId];
      a->capacity=G.stationCapacity[p->stationId]; a->denyReason=5; // DENIED (no slots)
      Transport::broadcast(buf,sizeof(buf));
      break;
    }

    // Accept hold
    G.holds[hi].active     = true;
    G.holds[hi].holdId     = p->holdId;
    G.holds[hi].stationId  = p->stationId;
    G.holds[hi].playerIdx  = pi;
    G.holds[hi].nextTickAt = now + (G.lootRateMs ? G.lootRateMs : 250); // safe fallback

    uint8_t buf[sizeof(MsgHeader)+sizeof(LootHoldAckPayload)];
    packHeader(G, (uint8_t)MsgType::LOOT_HOLD_ACK, sizeof(LootHoldAckPayload), buf, h->seq);
    auto* a=(LootHoldAckPayload*)(buf+sizeof(MsgHeader));
    a->holdId=p->holdId; a->accepted=1; a->rateHz=rateHz; a->maxCarry=G.maxCarry;
    a->carried=G.players[pi].carried;
    a->inventory=G.stationInventory[p->stationId];
    a->capacity =G.stationCapacity[p->stationId];
    a->denyReason=0;
    Transport::broadcast(buf,sizeof(buf));
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

      sendDropResult(G, dropped);
      bcastScore(G);
      break;
    }

    default: break;
  }
}
