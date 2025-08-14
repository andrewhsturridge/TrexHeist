#define TREX_TRANSPORT_ESPNOW
#include <Arduino.h>
#include <TREX_Shared/Protocol.h>
#include <TREX_Shared/Transport.h>

constexpr uint8_t  STATION_ID   = 6;   // drop-off
constexpr uint8_t  WIFI_CHANNEL = 6;

static uint16_t seq = 1;
static uint32_t lastHello = 0;

void sendHello() {
  uint8_t buf[sizeof(MsgHeader) + sizeof(HelloPayload)];
  auto* h = (MsgHeader*)buf;
  h->version = TREX_PROTO_VERSION;
  h->type = (uint8_t)MsgType::HELLO;
  h->srcStationId = STATION_ID;
  h->flags = 0;
  h->payloadLen = sizeof(HelloPayload);
  h->seq = seq++;

  auto* p = (HelloPayload*)(buf + sizeof(MsgHeader));
  p->stationType = (uint8_t)StationType::DROP;
  p->stationId   = STATION_ID;
  p->fwMajor = 0; p->fwMinor = 1;
  p->wifiChannel = WIFI_CHANNEL;
  memset(p->mac, 0, 6);

  Transport::sendToServer(buf, sizeof(buf));
}

void onRx(const uint8_t* data, uint16_t len) {
  if (len < sizeof(MsgHeader)) return;
  auto* h = (const MsgHeader*)data;
  if (h->version != TREX_PROTO_VERSION) return;

  if (h->type == (uint8_t)MsgType::STATE_TICK && h->payloadLen == sizeof(StateTickPayload)) {
    auto* p = (const StateTickPayload*)(data + sizeof(MsgHeader));
    Serial.printf("[DROP] %s  (%lu ms left)\n",
      (p->state == (uint8_t)LightState::GREEN) ? "GREEN" : "RED", (unsigned long)p->msLeft);
  }
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n[DROP] Boot");

  TransportConfig cfg{ /*maintenanceMode=*/false, /*wifiChannel=*/WIFI_CHANNEL };
  if (!Transport::init(cfg, onRx)) {
    Serial.println("[DROP] Transport init FAILED");
    while (1) delay(1000);
  }
}

void loop() {
  Transport::loop();
  uint32_t now = millis();
  if (now - lastHello > 1000) { sendHello(); lastHello = now; }
}
