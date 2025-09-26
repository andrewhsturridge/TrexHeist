#include "IdentitySerial.h"
#include "Identity.h"
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

void processIdentitySerial() {
  static char buf[96]; static size_t len = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[len] = 0;

      if (strcmp(buf, "whoami") == 0) {
        Serial.printf("[ID] id=%u host=%s\n", STATION_ID, HOSTNAME);

      } else if (!strncmp(buf, "id ", 3)) {
        int id = atoi(buf+3);
        if (id >= 1 && id <= 5) {
          saveIdentity((uint8_t)id, HOSTNAME);
          Serial.printf("[ID] Saved id=%d (host=%s). Rebooting…\n", id, HOSTNAME);
          delay(200); ESP.restart();
        } else {
          Serial.println("[ID] Usage: id <1..5>");
        }

      } else if (!strncmp(buf, "host ", 5)) {
        const char* h = buf+5;
        if (*h) {
          saveIdentity(STATION_ID, h);
          Serial.printf("[ID] Saved host=%s (id=%u). Rebooting…\n", h, STATION_ID);
          delay(200); ESP.restart();
        } else {
          Serial.println("[ID] Usage: host <name>");
        }

      } else if (!strncmp(buf, "ident ", 6)) {
        int id = 0; char name[32] = {0};
        if (sscanf(buf+6, "%d %31s", &id, name) == 2 && id>=1 && id<=5) {
          saveIdentity((uint8_t)id, name);
          Serial.printf("[ID] Saved id=%d host=%s. Rebooting…\n", id, name);
          delay(200); ESP.restart();
        } else {
          Serial.println("[ID] Usage: ident <1..5> <name>");
        }

      } else if (len) {
        Serial.println("[ID] cmds: whoami | id <1..5> | host <name> | ident <1..5> <name>");
      }

      len = 0;
      continue;
    }
    if ((c == 8 || c == 127) && len > 0) { len--; continue; }
    if (len < sizeof(buf)-1) buf[len++] = c;
  }
}
