#include "Identity.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

static Preferences idstore;

uint8_t STATION_ID = 0;          // loaded from NVS
char    HOSTNAME[32] = "Loot-0"; // loaded from NVS

void loadIdentity() {
  idstore.begin("trex", true);                 // read-only
  STATION_ID = idstore.getUChar("id", 0);
  String h   = idstore.getString("host", "Loot-0");
  idstore.end();
  strlcpy(HOSTNAME, h.c_str(), sizeof(HOSTNAME));
}

void saveIdentity(uint8_t id, const char* host) {
  idstore.begin("trex", false);                // write
  idstore.putUChar("id", id);
  idstore.putString("host", host);
  idstore.end();
}

void ensureIdentity() {
  loadIdentity();
  if (STATION_ID == 0 || HOSTNAME[0] == '\0' || !strncmp(HOSTNAME, "Loot-0", 6)) {
    // Derive a stable default from EFUSE MAC: map to 1..5
    uint8_t derived = (uint8_t)(ESP.getEfuseMac() & 0xFF);
    uint8_t id = (derived % 5) + 1;
    char host[32];
    snprintf(host, sizeof(host), "Loot-%u", id);
    saveIdentity(id, host);
    loadIdentity();
    Serial.printf("[ID] Auto-provisioned -> id=%u host=%s (from EFUSE MAC)\n", STATION_ID, HOSTNAME);
  } else {
    Serial.printf("[ID] Loaded -> id=%u host=%s\n", STATION_ID, HOSTNAME);
  }
}
