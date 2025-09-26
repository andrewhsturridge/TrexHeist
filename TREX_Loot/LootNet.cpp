#include "LootNet.h"
#include <Arduino.h>
#include <TrexTransport.h>
#include <TrexVersion.h>     // TREX_FW_MAJOR / TREX_FW_MINOR
#include "Identity.h"        // STATION_ID

#include <esp_random.h>      // esp_random for holdId

// Provided by the sketch:
extern uint8_t WIFI_CHANNEL;  // defined in TREX_Loot.ino
extern volatile bool holdActive;   // set true after accepted ACK; false on HOLD_END
extern uint32_t      holdId;       // current holdId, 0 when none

// Local message sequence number, like in the .ino
static uint16_t g_seq = 1;

/* ── pack header (moved) ─────────────────────────────── */
void packHeader(uint8_t type, uint16_t payLen, uint8_t* buf) {
  auto* h = (MsgHeader*)buf;
  h->version     = TREX_PROTO_VERSION;
  h->type        = type;
  h->srcStationId= STATION_ID;
  h->flags       = 0;
  h->payloadLen  = payLen;
  h->seq         = g_seq++;
}

/* ── NET: messages (moved) ───────────────────────────── */
void sendHello() {
  uint8_t buf[sizeof(MsgHeader)+sizeof(HelloPayload)];
  packHeader((uint8_t)MsgType::HELLO, sizeof(HelloPayload), buf);
  auto* p = (HelloPayload*)(buf + sizeof(MsgHeader));
  p->stationType = (uint8_t)StationType::LOOT;
  p->stationId   = STATION_ID;
  p->fwMajor     = TREX_FW_MAJOR;
  p->fwMinor     = TREX_FW_MINOR;
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
  auto* p = (LootHoldStopPayload*)(buf + sizeof(MsgHeader));
  p->holdId = holdId;
  Transport::sendToServer(buf, sizeof(buf));
  holdActive = false;
  holdId = 0;
}
