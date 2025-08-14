#define TREX_TRANSPORT_ESPNOW
#include <Arduino.h>
#include <TrexProtocol.h>
#include <TrexTransport.h>

constexpr uint8_t  STATION_ID   = 0;   // server
constexpr uint8_t  WIFI_CHANNEL = 6;
constexpr uint32_t GREEN_MS     = 5000;
constexpr uint32_t RED_MS       = 3000;

static uint16_t seq = 1;
static LightState state = LightState::GREEN;
static uint32_t nextSwitchMs;

void sendStateTick(uint32_t msLeft) {
  uint8_t buf[sizeof(MsgHeader) + sizeof(StateTickPayload)];
  auto* h = (MsgHeader*)buf;
  h->version = TREX_PROTO_VERSION;
  h->type = (uint8_t)MsgType::STATE_TICK;
  h->srcStationId = STATION_ID;
  h->flags = 0;
  h->payloadLen = sizeof(StateTickPayload);
  h->seq = seq++;

  auto* p = (StateTickPayload*)(buf + sizeof(MsgHeader));
  p->state = (uint8_t)state;
  p->msLeft = msLeft;

  Transport::broadcast(buf, sizeof(buf));
}

void onRx(const uint8_t* data, uint16_t len) {
  if (len < sizeof(MsgHeader)) return;
  auto* h = (const MsgHeader*)data;
  // For now, just print HELLOs so we know stations are alive
  if (h->version != TREX_PROTO_VERSION) return;
  if (h->type == (uint8_t)MsgType::HELLO) {
    Serial.printf("[TREX] HELLO from station %u (seq=%u)\n", h->srcStationId, h->seq);
  }
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n[TREX] Server boot");

  TransportConfig cfg{ /*maintenanceMode=*/false, /*wifiChannel=*/WIFI_CHANNEL };
  if (!Transport::init(cfg, onRx)) {
    Serial.println("[TREX] Transport init FAILED");
    while (1) delay(1000);
  }

  nextSwitchMs = millis() + GREEN_MS;
}

void loop() {
  Transport::loop();

  uint32_t now = millis();
  uint32_t msLeft = (nextSwitchMs > now) ? (nextSwitchMs - now) : 0;
  static uint32_t lastTick = 0;

  // broadcast ~5 Hz
  if (now - lastTick >= 200) {
    sendStateTick(msLeft);
    lastTick = now;
  }

  // flip GREEN/RED
  if (now >= nextSwitchMs) {
    state = (state == LightState::GREEN) ? LightState::RED : LightState::GREEN;
    nextSwitchMs = now + ((state == LightState::GREEN) ? GREEN_MS : RED_MS);
    Serial.printf("[TREX] -> %s\n", state == LightState::GREEN ? "GREEN" : "RED");
  }
}
