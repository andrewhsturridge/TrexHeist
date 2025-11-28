#include "OtaCampaign.h"
#include <Arduino.h>
#include <TrexProtocol.h>
#include <esp_random.h>

// Provided by Net.cpp (see shim above)
extern void netBroadcastRaw(const uint8_t* data, uint16_t len);

namespace OtaCampaign {

static uint32_t     g_campaignId = 0;
static uint32_t     g_startedMs  = 0;
static const uint32_t CAMPAIGN_TIMEOUT_MS = 120000; // 2 minutes
static uint8_t      g_expectMajor = 0, g_expectMinor = 0;

// StationState is defined in OtaCampaign.h, with fields:
// phase, error, fwMajor, fwMinor, bytes, total
static StationState g_state[6]; // index by stationId 0..5 (we use 1..5)
static bool         g_active = false;

// NEW: 0 = all loot, else specific STATION_ID
static uint8_t g_lootTargetId = 0;

void setLootTargetId(uint8_t targetId) {
  g_lootTargetId = targetId;
}

void begin() {
  memset(g_state, 0, sizeof(g_state));
  g_active = false;
  g_campaignId = 0;
}

void summary(const char* why) {
  Serial.println();
  Serial.printf("[OTA] Summary (%s) campaign=%lu  expect=%u.%u\n",
                why,
                (unsigned long)g_campaignId,
                g_expectMajor,
                g_expectMinor);
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
  if (!url || !*url) {
    Serial.println("[OTA] sendLootOtaToAll: empty URL, aborting");
    return;
  }

  g_campaignId  = (uint32_t)esp_random();
  g_expectMajor = expectMajor;
  g_expectMinor = expectMinor;
  g_startedMs   = millis();
  memset(g_state, 0, sizeof(g_state));
  g_active = true;

  uint8_t buf[sizeof(MsgHeader)+sizeof(ConfigUpdatePayload)];
  auto* h = (MsgHeader*)buf;
  auto* p = (ConfigUpdatePayload*)(buf + sizeof(MsgHeader));

  h->version      = TREX_PROTO_VERSION;
  h->type         = (uint8_t)MsgType::CONFIG_UPDATE;
  h->srcStationId = 0;
  h->flags        = 0;
  h->payloadLen   = sizeof(ConfigUpdatePayload);
  h->seq          = 0;

  p->stationType = (uint8_t)StationType::LOOT;
  // *** KEY CHANGE: use g_lootTargetId instead of always targeting all ***
  p->targetId    = g_lootTargetId; // 0 = all Loots, else specific STATION_ID
  memset(p->otaUrl, 0, sizeof(p->otaUrl));
  strlcpy(p->otaUrl, url, sizeof(p->otaUrl));
  p->campaignId  = g_campaignId;
  p->expectMajor = expectMajor;
  p->expectMinor = expectMinor;

  netBroadcastRaw(buf, sizeof(buf));
  Serial.printf("[OTA] Broadcast campaign=%lu url=%s expect=%u.%u targetId=%u\n",
    (unsigned long)g_campaignId, url, expectMajor, expectMinor,
    (unsigned)g_lootTargetId);
}

bool handle(const uint8_t* data, uint16_t len) {
  if (len < sizeof(MsgHeader)) return false;
  auto* h = (const MsgHeader*)data;
  if (h->version != TREX_PROTO_VERSION) return false;

  // ---- Case 1: OTA_STATUS (keep existing behavior) ----
  if ((MsgType)h->type == MsgType::OTA_STATUS) {
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

    // Early finish if everyone succeeded
    if (p->phase == (uint8_t)OtaPhase::SUCCESS) {
      bool allDone = true;
      for (int i=1;i<=5;i++)
        if (g_state[i].phase != (uint8_t)OtaPhase::SUCCESS) { allDone=false; break; }
      if (allDone) { summary("complete"); g_active=false; }
    }
    return true; // consumed
  }

  // ---- Case 2: HELLO (count as SUCCESS if version matches) ----
  if (!g_active) return false; // only track during an active campaign
  if ((MsgType)h->type == MsgType::HELLO) {
    if (h->payloadLen != sizeof(HelloPayload)) return false;
    auto* p = (const HelloPayload*)(data + sizeof(MsgHeader));
    if (p->stationType != (uint8_t)StationType::LOOT) return false;

    uint8_t id = p->stationId;
    if (id < 1 || id > 5) return false;

    // Version match rule: 0 = wildcard (ignore that field)
    bool majOK = (g_expectMajor == 0) || (p->fwMajor == g_expectMajor);
    bool minOK = (g_expectMinor == 0) || (p->fwMinor == g_expectMinor);

    StationState &s = g_state[id];
    s.fwMajor = p->fwMajor;
    s.fwMinor = p->fwMinor;

    if (majOK && minOK) {
      if (s.phase != (uint8_t)OtaPhase::SUCCESS) {
        s.phase = (uint8_t)OtaPhase::SUCCESS; s.error = 0; s.bytes = 0; s.total = 0;
        Serial.printf("[OTA] Loot-%u SUCCESS via HELLO v=%u.%u\n", id, p->fwMajor, p->fwMinor);

        bool allDone = true;
        for (int i=1;i<=5;i++)
          if (g_state[i].phase != (uint8_t)OtaPhase::SUCCESS) { allDone=false; break; }
        if (allDone) { summary("complete"); g_active=false; }
      }
      // return false so the rest of your app can also use HELLO normally
    } else {
      Serial.printf("[OTA] Loot-%u HELLO v=%u.%u (expected %u.%u) – not counting as success\n",
                    id, p->fwMajor, p->fwMinor, g_expectMajor, g_expectMinor);
    }
    return false; // let the rest of the server handle HELLO too
  }

  return false; // not an OTA-related message
}

} // namespace OtaCampaign
