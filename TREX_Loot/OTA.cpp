#include "OTA.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include "LootNet.h"      // packHeader(...)
#include "Identity.h"     // STATION_ID
#include <TrexVersion.h>  // TREX_FW_MAJOR / TREX_FW_MINOR
#include "TrexTransport.h"   // Transport::sendToServer, packHeader(), etc.

// ---- externs provided by the main sketch / other modules ----
extern const char* WIFI_SSID;  // from TREX_Loot.ino
extern const char* WIFI_PASS;  // from TREX_Loot.ino
extern bool transportReady;           // true once ESPNOW transport is ready
extern bool otaInProgress;            // guard to mute status sends during actual ArduinoOTA
extern uint32_t otaCampaignId;        // current campaign id (persisted to /ota.json)
extern uint8_t STATION_ID;            // station identity (from Identity module)
extern void otaTickSpinner();         // UI spinner during blocking OTA
extern void otaDrawProgress(uint32_t got, uint32_t total); // progress bar
extern void otaVisualFail();          // show failure visuals
extern void otaVisualSuccess();       // show success visuals

// ── Tunable timeouts for iPhone hotspot OTA ─────────────────
static constexpr uint32_t OTA_WIFI_CONNECT_TIMEOUT_MS      = 60000; // was 15000
static constexpr uint32_t OTA_HTTP_TIMEOUT_MS              = 30000; // was 15000
static constexpr uint32_t OTA_STREAM_INACTIVITY_TIMEOUT_MS = 30000; // was 15000

// WIFI_SSID / WIFI_PASS are expected to be defined in your build or config headers.

// ── OTA status send ───────────────────────────────────
void sendOtaStatus(OtaPhase phase, uint8_t errCode, uint32_t bytes, uint32_t total) {
  if (otaInProgress) return;
  if (!transportReady) { return; }
  uint8_t buf[sizeof(MsgHeader)+sizeof(OtaStatusPayload)];
  packHeader((uint8_t)MsgType::OTA_STATUS, sizeof(OtaStatusPayload), buf);
  auto* p = (OtaStatusPayload*)(buf + sizeof(MsgHeader));
  p->stationType = (uint8_t)StationType::LOOT;
  p->stationId   = STATION_ID;
  p->campaignId  = otaCampaignId;
  p->phase       = (uint8_t)phase;
  p->error       = errCode;
  p->fwMajor     = TREX_FW_MAJOR;  p->fwMinor = TREX_FW_MINOR;   // ← bump when you release
  p->bytes       = bytes;
  p->total       = total;
  Transport::sendToServer(buf, sizeof(buf));
}

// ── OTA persistence (/ota.json) ───────────────────────
void otaWriteFile(bool successPending) {
  StaticJsonDocument<128> d;
  d["campaignId"] = otaCampaignId;
  d["successPending"] = successPending ? 1 : 0;
  File f = LittleFS.open("/ota.json", "w");
  if (!f) return;
  serializeJson(d, f);
  f.close();
}

bool otaReadFile(uint32_t &campId, bool &successPending) {
  File f = LittleFS.open("/ota.json", "r");
  if (!f) return false;
  StaticJsonDocument<128> d;
  if (deserializeJson(d, f)) { f.close(); return false; }
  f.close();
  campId = d["campaignId"] | 0;
  successPending = (int)(d["successPending"] | 0) != 0;
  return true;
}

void otaClearFile() { LittleFS.remove("/ota.json"); }

// Small helper: handle FAIL + reboot in one place
static bool otaFailAndReboot(uint8_t errCode, uint32_t bytes, uint32_t total, const char* logMsg) {
  if (logMsg && *logMsg) {
    Serial.println(logMsg);
  }
  sendOtaStatus(OtaPhase::FAIL, errCode, bytes, total);
  otaVisualFail();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(200);
  ESP.restart();
  return false;  // not actually reached, but keeps compiler happy
}

// ── Do the OTA (blocking in STA) ──────────────────────
bool doOtaFromUrlDetailed(const char* url) {
  Serial.printf("[OTA] URL: %s\n", url);
  sendOtaStatus(OtaPhase::STARTING, 0, 0, 0);   // will be muted if otaInProgress guard is active

  // ---- Join Wi-Fi (STA) ----
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > OTA_WIFI_CONNECT_TIMEOUT_MS) {
      return otaFailAndReboot(1, 0, 0, "[OTA] WiFi connect timeout");
    }
    otaTickSpinner();
    delay(100);
  }
  Serial.printf("[OTA] WiFi connected: ch=%d  ip=%s\n",
                WiFi.channel(), WiFi.localIP().toString().c_str());

  // ---- HTTP request (no keep-alive) ----
  HTTPClient http;
  WiFiClient client;
  http.setReuse(false);       // force Connection: close
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);     // longer HTTP timeout

  if (!http.begin(client, url)) {
    http.end();
    return otaFailAndReboot(2, 0, 0, "[OTA] http.begin failed");
  }

  int code = http.GET();
  if (code != 200) {
    char msg[64];
    snprintf(msg, sizeof(msg), "[OTA] HTTP code %d", code);
    http.end();
    return otaFailAndReboot(2, 0, 0, msg);
  }

  int total = http.getSize();         // -1 if server didn’t send Content-Length
  Serial.printf("[OTA] total bytes: %d\n", total);

  // ---- Begin flash ----
  if (total > 0) {
    if (!Update.begin(total)) {
      char msg[96];
      snprintf(msg, sizeof(msg), "[OTA] Update.begin failed: %s (need %d)", Update.errorString(), total);
      http.end();
      return otaFailAndReboot(4, 0, (uint32_t)total, msg);
    }
  } else {
    if (!Update.begin()) {
      char msg[96];
      snprintf(msg, sizeof(msg), "[OTA] Update.begin failed (no len): %s", Update.errorString());
      http.end();
      return otaFailAndReboot(4, 0, 0, msg);
    }
  }

  WiFiClient* stream = http.getStreamPtr();
  const size_t BUF = 2048;
  uint8_t buf[BUF];
  uint32_t got = 0;
  uint32_t lastActivity = millis();
  uint32_t lastDraw = 0;

  // Read until we’ve received Content-Length (or EOF if none), with inactivity timeout.
  while ( (total < 0) || (got < (uint32_t)total) ) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = (avail > BUF) ? BUF : avail;
      int read = stream->readBytes((char*)buf, toRead);
      if (read <= 0) { delay(1); continue; }

      size_t wrote = Update.write(buf, (size_t)read);
      if (wrote != (size_t)read) {
        char msg[96];
        snprintf(msg, sizeof(msg), "[OTA] Write error: %s at %lu/%d",
                 Update.errorString(), (unsigned long)got, total);
        http.end();
        return otaFailAndReboot(4, got, (uint32_t)(total>0?total:0), msg);
      }

      got += wrote;
      lastActivity = millis();

      // Smooth scheduler/Wi-Fi
      delay(0);

      // Draw progress (throttle to every 16 KB)
      if (total > 0 && (got - lastDraw) >= 16384) {
        otaDrawProgress(got, (uint32_t)total);
        lastDraw = got;
      }
    } else {
      // No data available right now
      otaTickSpinner();
      delay(1);

      // If we got a length and reached it, we’re done
      if (total > 0 && got >= (uint32_t)total) break;

      // If no length, consider EOF when socket closes
      if (total < 0 && !stream->connected() && stream->available() == 0) break;

      // Inactivity timeout
      if (millis() - lastActivity > OTA_STREAM_INACTIVITY_TIMEOUT_MS) {
        http.end();
        return otaFailAndReboot(2, got, (uint32_t)(total>0?total:0), "[OTA] Stream timeout (no data)");
      }
    }
  }

  // Finish & verify
  bool ok = Update.end(true);   // true = perform MD5/verify if supported
  http.end();

  if (!ok || !Update.isFinished()) {
    char msg[96];
    snprintf(msg, sizeof(msg), "[OTA] End/verify error: %s (wrote %lu/%d)",
             Update.errorString(), (unsigned long)got, total);
    return otaFailAndReboot(5, got, (uint32_t)(total>0?total:0), msg);
  }

  // Success → persist flag and reboot; SUCCESS will be reported after ESPNOW is up
  otaWriteFile(true);
  otaVisualSuccess();
  delay(200);
  ESP.restart();
  return true;  // not reached
}
