#pragma once
#include <stdint.h>

extern uint8_t STATION_ID;       // current station id (1..5)
extern char    HOSTNAME[32];     // current hostname, e.g. "Loot-3"

void loadIdentity();                     // read from NVS
void saveIdentity(uint8_t id, const char* host); // write to NVS
void ensureIdentity();                   // load, auto-provision if blank
