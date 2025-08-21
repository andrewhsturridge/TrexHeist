#pragma once
#include <Arduino.h>
#include <TrexProtocol.h>
#include <TrexTransport.h>

namespace OtaCampaign {

struct StationState {
  uint8_t  phase;    // OtaPhase (or 0=none)
  uint8_t  error;    // last error
  uint8_t  fwMajor;
  uint8_t  fwMinor;
  uint32_t bytes;
  uint32_t total;
};

void begin();
void loop();  // handles timeouts & periodic summary

// Broadcast to all LOOT stations
void sendLootOtaToAll(const char* url, uint8_t expectMajor, uint8_t expectMinor);

// Call from your server onRx() early; returns true if message was handled
bool handle(const uint8_t* data, uint16_t len);

} // namespace
