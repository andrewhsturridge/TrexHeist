#include <LittleFS.h>
#include <ArduinoJson.h>

struct StationCfg {
  uint8_t stationId   = 0;       // 1..5 for Loot stations
  uint8_t wifiChannel = 6;       // must match T-Rex / ESP-NOW
  char    hostname[32] = "Loot-0";
};

StationCfg g_cfg;

static bool saveStationCfg(const StationCfg& c) {
  if (!LittleFS.begin()) LittleFS.begin(true);
  File f = LittleFS.open("/station.json", "w");
  if (!f) return false;
  StaticJsonDocument<256> d;
  d["stationId"]   = c.stationId;
  d["wifiChannel"] = c.wifiChannel;
  d["hostname"]    = c.hostname;
  serializeJson(d, f);
  f.close();
  return true;
}

static bool loadStationCfg() {
  if (!LittleFS.begin()) LittleFS.begin(true);
  File f = LittleFS.open("/station.json", "r");
  if (!f) return false;
  StaticJsonDocument<256> d;
  if (deserializeJson(d, f)) { f.close(); return false; }
  f.close();
  g_cfg.stationId   = d["stationId"]   | 0;
  g_cfg.wifiChannel = d["wifiChannel"] | 6;
  strlcpy(g_cfg.hostname, (const char*)(d["hostname"] | "Loot-0"), sizeof(g_cfg.hostname));
  return true;
}
