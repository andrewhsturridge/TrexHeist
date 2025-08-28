/*
  TREX – T-Rex Server (Feather S3)
  Split architecture with Maintenance + Classic mode (Warmup → Levels)
*/

#include <Arduino.h>
#include <TrexProtocol.h>
#include <TrexTransport.h>
#include <TrexVersion.h>

#include "ServerConfig.h"
#include "GameModel.h"
#include "Cadence.h"
#include "ModeClassic.h"
#include "Net.h"
#include "Media.h"
#include "MaintCommands.h"
#include "TrexMaintenance.h"   // updated version with custom command hook
#include "OtaCampaign.h"
#include "GameAudio.h"

// --- OTA defaults (edit these per release) ---
#define DEFAULT_OTA_URL          "http://192.168.2.231:8000/TREX_Loot.ino.bin"
#define DEFAULT_OTA_EXPECT_MAJOR TREX_FW_MAJOR
#define DEFAULT_OTA_EXPECT_MINOR TREX_FW_MINOR

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

  gameAudioInit(/*rxPin=*/9, /*txPin=*/8, /*baud=*/9600, /*volume=*/25);

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
    if (c=='n' || c=='N') { startNewGame(g); }
    if (c=='g' || c=='G') {
      if (g.roundIndex == 0 || g.phase == Phase::END) {
        startNewGame(g);          // full reset + Round 1 + drip
      } else {
        enterGreen(g);            // mid-game manual flip stays supported
      }
    }
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
    if (c=='r' || c=='R') enterRed(g);
    if (c=='x' || c=='X') bcastGameOver(g, /*MANUAL*/2);

    // ---------- NEW: Round controls ----------
    if (c=='1') { modeClassicForceRound(g, 1, /*playWin=*/false); } // jump to R1 silently
    if (c=='2') { modeClassicForceRound(g, 2, /*playWin=*/true ); } // jump to R2 (play win sting)
    if (c=='3') { modeClassicForceRound(g, 3, /*playWin=*/true ); } // jump to R3 (play win sting)
    if (c=='4') { modeClassicForceRound(g, 4, /*playWin=*/true ); } // jump to R4 (play win sting)
    if (c=='>' || c=='.') { modeClassicNextRound(g, /*playWin=*/true); }
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
    uint32_t msLeft = (g.roundIndex==1) ? ((g.roundEndAt>now)?(g.roundEndAt-now):0) : ((g.gameEndAt>now)?(g.gameEndAt-now):0);
    sendStateTick(g, msLeft);
    g.lastTickSentMs = now;
  }

  // Accrual while GREEN (tick every lootRateMs; grant lootPerTick each tick)
  if (g.phase == Phase::PLAYING && g.light == LightState::GREEN) {
    for (int i = 0; i < MAX_HOLDS; ++i) if (g.holds[i].active) {
      auto &h  = g.holds[i];
      uint8_t sid = h.stationId;
      if (sid < 1 || sid > 5) { h.active = false; continue; }   // safety

      auto &pl = g.players[h.playerIdx];

      if ((int32_t)(now - h.nextTickAt) >= 0) {
        const uint32_t period  = (g.lootRateMs ? g.lootRateMs : 1000U);
        uint8_t  room  = (pl.carried >= g.maxCarry) ? 0 : (uint8_t)(g.maxCarry - pl.carried);
        uint16_t avail = g.stationInventory[sid];

        // Grant N items this tick, but never exceed carry cap or inventory
        uint16_t grant = g.lootPerTick;
        if (grant > room)  grant = room;
        if (grant > avail) grant = avail;

        if (grant == 0) {
          // No room or no inventory → close the hold with a clear reason
          h.active = false;
          sendHoldEnd(g, h.holdId, (avail == 0) ? /*EMPTY*/1 : /*FULL*/0);
          h.nextTickAt += period;   // keep schedule monotonic
          continue;
        }

        pl.carried              = (uint8_t)(pl.carried + grant);
        g.stationInventory[sid] = (uint16_t)(g.stationInventory[sid] - grant);

        // Notify player + everyone else (now at tick period cadence)
        sendLootTick(g, h.holdId, pl.carried, g.stationInventory[sid]);
        bcastStation(g, sid);

        h.nextTickAt += period;
        // If you want catch-up on long stalls, change the 'if' to a 'while' loop.
      }
    }
  }

  // Linger holds into RED? After grace, it's a violation.
  if (g.phase == Phase::PLAYING && g.light == LightState::RED) {
    if (now >= g.redGraceUntil) {
      bool anyHold = false;
      for (auto &h : g.holds) if (h.active) { anyHold = true; break; }
      if (anyHold) {
        bcastGameOver(g, /*RED_LOOT*/1);
        return;
      }
    }
  }

  // PIR violation during RED (after arming delay)
  if (g.phase == Phase::PLAYING && g.light == LightState::RED && g.pirEnforce) {
    if (now >= g.pirArmAt) {
      for (int i = 0; i < 4; ++i) {
        int pin = g.pir[i].pin;
        if (pin >= 0) {
          bool trig = (digitalRead(pin) == LOW);  // active-LOW
          // optional: track state/time
          g.pir[i].state = trig;
          if (trig) {
            bcastGameOver(g, /*RED_PIR*/3);
            return;
          }
        }
      }
    }
  }

  // Level progression (Classic mode)
  if (g.phase == Phase::PLAYING) {
    modeClassicMaybeAdvance(g);
  }

  tickCadence(g, now);
}
