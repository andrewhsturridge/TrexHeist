#include "ServerMini.h"
#include <Arduino.h>
#include <TrexProtocol.h>
#include <TrexTransport.h>
#include <TrexVersion.h>
#include <esp_random.h>

// Your server state
#include "Game.h"          // make sure this exposes Game&
                           // add MgState mg; field to Game (see note below)

// ---- Local helpers ---------------------------------------------------------
static inline uint32_t bitForSid(uint8_t sid) {
  if (sid == 0 || sid >= 32) return 0;
  return (1u << sid);
}

static void packHeader(uint8_t type, uint16_t payLen, uint8_t* buf) {
  auto* h = reinterpret_cast<MsgHeader*>(buf);
  h->version      = TREX_PROTO_VERSION;
  h->type         = type;
  h->srcStationId = 0;                // 0 = server
  h->flags        = 0;
  h->payloadLen   = payLen;
  static uint16_t seq = 1;
  h->seq = seq++;
}

static void bcastMgStart(const MgConfig& c) {
  uint8_t buf[sizeof(MsgHeader) + sizeof(MgStartPayload)];
  packHeader((uint8_t)MsgType::MG_START, sizeof(MgStartPayload), buf);
  auto* p = reinterpret_cast<MgStartPayload*>(buf + sizeof(MsgHeader));
  p->seed       = c.seed;
  p->timerMs    = c.timerMs;
  p->speedMinMs = c.speedMinMs;
  p->speedMaxMs = c.speedMaxMs;
  p->segMin     = c.segMin;
  p->segMax     = c.segMax;
  Transport::broadcast(buf, sizeof buf);
}

static void bcastMgStop() {
  uint8_t buf[sizeof(MsgHeader)];
  packHeader((uint8_t)MsgType::MG_STOP, 0, buf);
  Transport::broadcast(buf, sizeof buf);
}

// ---- Integration: awarding a point to a UID --------------------------------
// Replace this stub with your real scoring hook if you have one.
static void awardBonusPoint(Game& g, const TrexUid& uid) {
  // Example: bump team score OR maintain per-UID credit
  // g.teamScore += 1;
  // OR: g.bonusByUid.addOrIncrement(uid);
  Serial.printf("[MG] +1 bonus point for UID (len=%u)\n", uid.len);
}

// ---- Public API ------------------------------------------------------------
void MG_Init(Game& g) {
  g.mg = MgState{};
}

void MG_Start(Game& g, const MgConfig& cfg, uint32_t nowMs) {
  if (g.mg.active) return;

  MgConfig c = cfg;
  if (c.seed == 0) c.seed = esp_random();
  if (c.timerMs == 0) c.timerMs = 60000;
  if (c.speedMinMs == 0) c.speedMinMs = 20;
  if (c.speedMaxMs == 0) c.speedMaxMs = 80;
  if (c.segMin == 0) c.segMin = 6;
  if (c.segMax == 0) c.segMax = 16;

  g.mg.active         = true;
  g.mg.startedAt      = nowMs;
  g.mg.deadline       = nowMs + c.timerMs;
  g.mg.allUsedAt      = 0;
  g.mg.triedMask      = 0;
  g.mg.successMask    = 0;
  g.mg.resultMask     = 0;
  g.mg.cfg            = c;

  // Broadcast start to clients
  bcastMgStart(c);
  Serial.printf("[MG] START seed=%lu timer=%u speed=%u..%u seg=%u..%u\n",
                (unsigned long)c.seed, c.timerMs, c.speedMinMs, c.speedMaxMs, c.segMin, c.segMax);
}

bool MG_Tick(Game& g, uint32_t nowMs) {
  if (!g.mg.active) return false;

  // Timer expiry?
  if ((int32_t)(nowMs - g.mg.deadline) >= 0) {
    MG_Stop(g, nowMs);
    return false;
  }

  // All stations tried → wait extra 3s then stop
  if (g.mg.allUsedAt && (int32_t)(nowMs - (g.mg.allUsedAt + 3000)) >= 0) {
    MG_Stop(g, nowMs);
    return false;
  }

  return true; // still running; caller should avoid advancing rounds
}

void MG_OnResult(Game& g, const MgResultPayload& r, uint32_t nowMs) {
  if (!g.mg.active) return;

  const uint32_t bit = bitForSid(r.stationId);
  if (bit == 0) return;

  // one try per station
  if (g.mg.triedMask & bit) {
    Serial.printf("[MG] duplicate result from sid=%u ignored\n", r.stationId);
    return;
  }

  g.mg.triedMask  |= bit;
  g.mg.resultMask |= bit;
  if (r.success) g.mg.successMask |= bit;

  if (r.success) awardBonusPoint(g, r.uid);

  // If everyone we expect has tried, arm the 3s grace
  // NOTE: expectedStations is a count; we compare bitcount of triedMask to it.
  uint32_t m = g.mg.triedMask;
  uint8_t count = 0;
  while (m) { count += (m & 1u); m >>= 1; }
  if (count >= g.mg.expectedStations && g.mg.allUsedAt == 0) {
    g.mg.allUsedAt = nowMs;
  }

  Serial.printf("[MG] result sid=%u success=%u triedMask=%08lx\n",
                r.stationId, r.success ? 1 : 0, (unsigned long)g.mg.triedMask);
}

void MG_Stop(Game& g, uint32_t /*nowMs*/) {
  if (!g.mg.active) return;
  g.mg.active = false;
  bcastMgStop();
  Serial.println("[MG] STOP");
}
