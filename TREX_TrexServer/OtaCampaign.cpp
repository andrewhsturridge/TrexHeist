#include "OtaCampaign.h"
#include <TrexProtocol.h>

// Provided by Net.cpp (see shim above)
extern void netBroadcastRaw(const uint8_t* data, uint16_t len);

namespace OtaCampaign {

static uint32_t     g_campaignId = 0;
static uint32_t     g_startedMs  = 0;
static const uint32_t CAMPAIGN_TIMEOUT_MS = 120000; // 2 minutes
static uint8_t      g_expectMajor = 0, g_expectMinor = 0;

static StationState g_state[6]; // index by stationId 0..5 (we use 1..5)
static bool         g_active = false;

void begin() {
  memset(g_state, 0, sizeof(g_state));
  g_active = false;
  g_campaignId = 0;
}

void summary(const char* why) {
  Serial.println();
  Serial.printf("[OTA] Summary (%s) campaign=%lu\n", why, (unsigned long)g_campaignId);
  for (int id=1; id<=5; ++id) {
    StationState &s = g_state[id];
    const char* ph = (s.phase==0)?"PENDING":
                     (s.phase==(uint8_t)OtaPhase::ACK)?"ACK":
                     (s.phase==(uint8_t)OtaPhase::STARTING)?"STARTING":
                     (s.phase==(uint8_t)OtaPhase::FAIL)?"FAIL":
                     (s.phase==(uint8_t)OtaPhase::SUCCESS)?"SUCCESS":"?";
    Serial.printf("  Loot-%d: %-9s  err=%u  v=%u.%u  %lu/%lu\n",
      id, ph, s.error, s.fwMajor, s.fwMinor,
      (unsigned long)s.bytes, (unsigned long)s.total);
  }
  Serial.println();
}

void loop() {
  if (!g_active) return;
  if (millis() - g_startedMs > CAMPAIGN_TIMEOUT_MS) {
    summary("timeout");
    g_active = false;
  }
}

void sendLootOtaToAll(const char* url, uint8_t expectMajor, uint8_t expectMinor) {
  g_campaignId = (uint32_t)esp_random();
  g_expectMajor = expectMajor; g_expectMinor = expectMinor;
  g_startedMs = millis();
  memset(g_state, 0, sizeof(g_state));
  g_active = true;

  uint8_t buf[sizeof(MsgHeader)+sizeof(ConfigUpdatePayload)];
  auto* h = (MsgHeader*)buf;
  auto* p = (ConfigUpdatePayload*)(buf + sizeof(MsgHeader));

  h->version = TREX_PROTO_VERSION;
  h->type    = (uint8_t)MsgType::CONFIG_UPDATE;
  h->srcStationId = 0;
  h->flags = 0;
  h->payloadLen = sizeof(ConfigUpdatePayload);
  h->seq = 0;

  p->stationType = (uint8_t)StationType::LOOT;
  p->targetId    = 0; // all Loots
  memset(p->otaUrl, 0, sizeof(p->otaUrl));
  strlcpy(p->otaUrl, url, sizeof(p->otaUrl));
  p->campaignId  = g_campaignId;
  p->expectMajor = expectMajor;
  p->expectMinor = expectMinor;

  netBroadcastRaw(buf, sizeof(buf));
  Serial.printf("[OTA] Broadcast campaign=%lu url=%s expect=%u.%u\n",
    (unsigned long)g_campaignId, url, expectMajor, expectMinor);
}

bool handle(const uint8_t* data, uint16_t len) {
  if (len < sizeof(MsgHeader)) return false;
  auto* h = (const MsgHeader*)data;
  if (h->version != TREX_PROTO_VERSION) return false;
  if ((MsgType)h->type != MsgType::OTA_STATUS) return false;
  if (h->payloadLen != sizeof(OtaStatusPayload)) return true; // ignore malformed
  auto* p = (const OtaStatusPayload*)(data + sizeof(MsgHeader));
  if (p->stationType != (uint8_t)StationType::LOOT) return true;

  uint8_t id = p->stationId;
  if (id < 1 || id > 5) return true;

  StationState &s = g_state[id];
  s.phase  = p->phase;
  s.error  = p->error;
  s.fwMajor= p->fwMajor;
  s.fwMinor= p->fwMinor;
  s.bytes  = p->bytes;
  s.total  = p->total;

  const char* ph = (p->phase==(uint8_t)OtaPhase::ACK)?"ACK":
                   (p->phase==(uint8_t)OtaPhase::STARTING)?"STARTING":
                   (p->phase==(uint8_t)OtaPhase::FAIL)?"FAIL":
                   (p->phase==(uint8_t)OtaPhase::SUCCESS)?"SUCCESS":"?";
  Serial.printf("[OTA] Loot-%u %-8s err=%u v=%u.%u %lu/%lu\n",
    id, ph, p->error, p->fwMajor, p->fwMinor,
    (unsigned long)p->bytes, (unsigned long)p->total);

  // Early finish if everyone reported SUCCESS (or FAIL/TIMEOUT later)
  if (p->phase == (uint8_t)OtaPhase::SUCCESS) {
    bool allDone = true;
    for (int i=1;i<=5;i++) {
      if (g_state[i].phase != (uint8_t)OtaPhase::SUCCESS) { allDone = false; break; }
    }
    if (allDone) { summary("complete"); g_active=false; }
  }
  return true;
}

} // namespace
