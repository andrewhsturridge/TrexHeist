/*
  TREX – T-Rex Server (Feather S3)
  Split architecture with Maintenance + Classic mode (Warmup → Levels)
*/

#include <Arduino.h>
#include <TrexProtocol.h>
#include <TrexTransport.h>

#include "ServerConfig.h"
#include "GameModel.h"
#include "Cadence.h"
#include "ModeClassic.h"
#include "Net.h"
#include "Media.h"
#include "MaintCommands.h"
#include "TrexMaintenance.h"   // updated version with custom command hook
#include "OtaCampaign.h"


// --- OTA defaults (edit these per release) ---
#define DEFAULT_OTA_URL          "http://192.168.2.231:8000/TREX_Loot.ino.bin"
#define DEFAULT_OTA_EXPECT_MAJOR 0
#define DEFAULT_OTA_EXPECT_MINOR 3

Game g;

/* ── Setup ────────────────────────────────────────────────── */
void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(BOARD_BLUE_LED, OUTPUT);
  digitalWrite(BOARD_BLUE_LED, LOW);  // off by default

  Serial.println("\n[TREX] Server boot");

  OtaCampaign::begin();

  // PIR pins
  for (int i=0;i<4;i++) {
    g.pir[i].pin = PIN_PIR[i];
    if (g.pir[i].pin >= 0) {
      pinMode(g.pir[i].pin, INPUT_PULLUP);
      bool initState = (digitalRead(g.pir[i].pin) == LOW); // active-LOW
      g.pir[i].state = g.pir[i].last = initState;
      g.pir[i].lastChange = millis();
      Serial.printf("[TREX] PIR%d on GPIO%d init=%s\n", i, g.pir[i].pin, initState?"TRIG":"IDLE");
    }
  }

  mediaInit();  // Sprite serial etc.

  // Transport
  TransportConfig cfg{ /*maintenanceMode=*/false, /*wifiChannel=*/WIFI_CHANNEL };
  if (!Transport::init(cfg, onRx)) {
    Serial.println("[TREX] Transport init FAILED");
    while (1) delay(1000);
  }
  Serial.printf("Trex header ver: %d\n", TREX_PROTO_VERSION);

  // Game + Mode
  resetGame(g);
  modeClassicInit(g);   // Warmup enabled here
  enterGreen(g);        // starts GREEN; cadence flips are suppressed during warmup

  // Register server-specific Telnet commands (used only in maintenance)
  maintRegisterServerCommands(g);
}

/* ── Loop ─────────────────────────────────────────────────── */
void loop() {
  // ---- Maintenance entry/loop (paused mode) ----
  static Maint::Config mcfg{WIFI_SSID, WIFI_PASS, HOSTNAME,
                            /*apFallback=*/true, /*apChannel=*/WIFI_CHANNEL,
                            /*apPass=*/"trexsetup", /*buttonPin=*/0, /*holdMs=*/1500};
  mcfg.stationType  = StationType::TREX;
  mcfg.stationId    = STATION_ID;
  mcfg.enableBeacon = true;

  // Track last LED state so we only write on transitions
  static bool maintLEDOn = false;

  // Enter via BOOT long-press
  bool justEntered = Maint::checkRuntimeEntry(mcfg);
  if (justEntered || Maint::active) {
    if (!maintLEDOn) { digitalWrite(BOARD_BLUE_LED, HIGH); maintLEDOn = true; }
    Maint::loop();
    return;
  } else if (maintLEDOn) {
    // Only turn it off once when leaving maintenance (if you ever add an exit)
    digitalWrite(BOARD_BLUE_LED, LOW);
    maintLEDOn = false;
  }

  OtaCampaign::loop();

  Transport::loop();

  uint32_t now = millis();

  // Maintenance shortcut via Serial
  while (Serial.available()) {
    int c = Serial.read();
    if (c=='m' || c=='M') { Maint::begin(mcfg); digitalWrite(BOARD_BLUE_LED, HIGH); return; }
    if (c=='n' || c=='N') startNewGame(g);
    if (c=='g' || c=='G') enterGreen(g);
    if (c=='r' || c=='R') enterRed(g);
    if (c=='x' || c=='X') bcastGameOver(g, /*MANUAL*/2);
    // Update all Loot stations (press 'U')
    if (c == 'u' || c == 'U') {
      // Ensure game is idle so Loots will accept OTA
      bcastGameOver(g, /*MANUAL*/2);
      Serial.println("[OTA] GAME_OVER sent; broadcasting in 3s…");

      // Simple grace period; keeps loops active while waiting
      uint32_t t0 = millis();
      while (millis() - t0 < 3000) {
        OtaCampaign::loop();
        Transport::loop();
        delay(10);
      }

      // Fire the OTA broadcast
      OtaCampaign::sendLootOtaToAll(
          DEFAULT_OTA_URL,
          DEFAULT_OTA_EXPECT_MAJOR,
          DEFAULT_OTA_EXPECT_MINOR
      );
    }
  }

  // Drip broadcast on new game start
  static uint32_t lastSend = 0;
  if (now - lastSend >= 50) { // ~20 msgs/sec
    if (g.pending.needGameStart) {
      bcastGameStart(g);
      g.pending.needGameStart = false;
      lastSend = now;
    } else if (g.pending.nextStation >= 1 && g.pending.nextStation <= 5) {
      bcastStation(g, g.pending.nextStation++);
      lastSend = now;
    } else if (g.pending.needScore) {
      bcastScore(g);
      g.pending.needScore = false;
      lastSend = now;
    }
  }

  // STATE_TICK @ tickHz
  const uint32_t tickMs = max<uint32_t>(10, 1000 / g.tickHz);
  if (now - g.lastTickSentMs >= tickMs) {
    uint32_t msLeft = 0;
    if (g.warmupActive) {
      msLeft = (g.warmupEndAt > now) ? (g.warmupEndAt - now) : 0;
    } else {
      msLeft = (g.nextSwitch > now) ? (g.nextSwitch - now) : 0;
    }
    sendStateTick(g, msLeft);
    g.lastTickSentMs = now;
  }

  // Cadence flips (suppressed during warmup)
  tickCadence(g, now);

  // PIR monitoring (RED only) with arm delay + optional enforcement
  if (!g.warmupActive && g.phase == Phase::PLAYING && g.light == LightState::RED && g.pirEnforce) {
    if (now >= g.pirArmAt) {
      bool anyTrig = false;
      for (int i=0;i<4;i++) if (g.pir[i].pin >= 0) {
        bool raw = (digitalRead(g.pir[i].pin) == LOW);
        if (raw != g.pir[i].last && (now - g.pir[i].lastChange) > PIR_DEBOUNCE_MS) {
          g.pir[i].last = raw; g.pir[i].lastChange = now; g.pir[i].state = raw;
          if (raw) { anyTrig = true; } // rising to TRIGGERED
        }
      }
      if (anyTrig) {
        bcastGameOver(g, /*RED_PIR*/0);
        return;
      }
    }
  }

  // Active holds (server-driven tick @ lootRate while GREEN)
  if (!g.warmupActive && g.phase == Phase::PLAYING && g.light == LightState::GREEN) {
    for (int i=0;i<MAX_HOLDS;i++) if (g.holds[i].active) {
      auto &h = g.holds[i];
      auto &pl = g.players[h.playerIdx];
      uint8_t sid = h.stationId;

      if (now >= h.nextTickAt) {
        if (pl.carried >= g.maxCarry) {
          h.active=false;
          sendHoldEnd(g, h.holdId, /*FULL*/0);
          continue;
        }
        if (g.stationInventory[sid] == 0) {
          h.active=false;
          sendHoldEnd(g, h.holdId, /*EMPTY*/1);
          continue;
        }

        // Apply +1/-1 for this tick
        pl.carried += 1;
        g.stationInventory[sid] -= 1;

        // Tick + station update
        sendLootTick(g, h.holdId, pl.carried, g.stationInventory[sid]);
        bcastStation(g, sid);

        h.nextTickAt += g.lootRateMs;
      }
    }
  }

  // Linger holds into RED? After grace, it's a violation.
  if (!g.warmupActive && g.phase == Phase::PLAYING && g.light == LightState::RED) {
    if (now >= g.redGraceUntil) {
      bool anyHold = false;
      for (auto &h : g.holds) if (h.active) { anyHold = true; break; }
      if (anyHold) {
        bcastGameOver(g, /*RED_LOOT*/1);
        return;
      }
    }
  }

  // Level progression (Classic mode)
  if (!g.warmupActive && g.phase == Phase::PLAYING) {
    modeClassicMaybeAdvance(g);
  }
}
