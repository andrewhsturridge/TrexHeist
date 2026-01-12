/*
  TREX – T-Rex Server (Feather S3)
  Split architecture with Maintenance + Classic mode (Warmup → Levels)
*/

#include <Arduino.h>
#include <TrexProtocol.h>
#include <TrexTransport.h>
#include <TrexVersion.h>
#include <Preferences.h>

#include "ServerConfig.h"
#include "GameModel.h"
#include "Cadence.h"
#include "ModeClassic.h"
#include "Net.h"
#include "Media.h"
#include "MaintCommands.h"
#include "TrexMaintenance.h"
#include "OtaCampaign.h"
#include "GameAudio.h"
#include "Bonus.h"

// --- OTA defaults (edit these per release) ---
#define DEFAULT_OTA_URL          "http://172.20.10.3:8000/TrexHeist/TREX_Loot/build/esp32.esp32.um_feathers3/TREX_Loot.ino.bin"
#define DEFAULT_OTA_EXPECT_MAJOR TREX_FW_MAJOR
#define DEFAULT_OTA_EXPECT_MINOR TREX_FW_MINOR

Game g;

// --- Radio config (persisted in NVS) ------------------------------------
// Keys live in Preferences namespace "trex":
//   chan = Wi-Fi channel (1..13)
//   txf  = TX framed (0/1)   (wire header / magic)
//   rxl  = RX accept legacy (0/1)
static uint8_t WIFI_CHANNEL     = DEFAULT_WIFI_CHANNEL;
static bool    TX_FRAMED        = false;  // false = legacy packets (no wire header)
static bool    RX_ACCEPT_LEGACY = true;   // true = accept packets without wire header

static void loadRadioConfig() {
  Preferences p;
  p.begin("trex", true);
  uint8_t ch  = p.getUChar("chan", DEFAULT_WIFI_CHANNEL);
  uint8_t txf = p.getUChar("txf",  0);
  uint8_t rxl = p.getUChar("rxl",  1);
  p.end();

  if (ch < 1 || ch > 13) ch = DEFAULT_WIFI_CHANNEL;
  WIFI_CHANNEL     = ch;
  TX_FRAMED        = (txf != 0);
  RX_ACCEPT_LEGACY = (rxl != 0);

  Serial.printf("[RADIO] Loaded: chan=%u txFramed=%u rxLegacy=%u",
                (unsigned)WIFI_CHANNEL,
                (unsigned)(TX_FRAMED ? 1 : 0),
                (unsigned)(RX_ACCEPT_LEGACY ? 1 : 0));
}

static void saveRadioConfig(uint8_t ch, bool txFramed, bool rxLegacy) {
  Preferences p;
  p.begin("trex", false);
  p.putUChar("chan", ch);
  p.putUChar("txf",  txFramed ? 1 : 0);
  p.putUChar("rxl",  rxLegacy ? 1 : 0);
  p.end();
}

// Broadcast + persist + reboot into new settings.
// Request format supports "no change":
//   wifiChannel: 0 => keep current
//   txFramed:   0/1 set, else keep current
//   rxLegacy:   0/1 set, else keep current
static void applyRadioCfgAndReboot(const RadioCfgPayload& req, const char* why) {
  uint8_t ch = WIFI_CHANNEL;
  bool txf = TX_FRAMED;
  bool rxl = RX_ACCEPT_LEGACY;

  if (req.wifiChannel >= 1 && req.wifiChannel <= 13) ch = req.wifiChannel;
  if (req.txFramed == 0 || req.txFramed == 1) txf = (req.txFramed != 0);
  if (req.rxLegacy == 0 || req.rxLegacy == 1) rxl = (req.rxLegacy != 0);

  RadioCfgPayload out{};
  out.wifiChannel = ch;
  out.txFramed    = txf ? 1 : 0;
  out.rxLegacy    = rxl ? 1 : 0;
  out._pad        = 0;

  Serial.printf("[RADIO] Apply (%s): chan=%u txFramed=%u rxLegacy=%u",
                why ? why : "?",
                (unsigned)out.wifiChannel,
                (unsigned)out.txFramed,
                (unsigned)out.rxLegacy);

  // Tell everyone first (while we're still on the old channel/mode)
  bcastRadioCfg(g, out);
  delay(150);

  // Persist + reboot (server is the source of truth)
  saveRadioConfig(out.wifiChannel, out.txFramed != 0, out.rxLegacy != 0);
  delay(150);
  ESP.restart();
}


void triggerLootOta(Game& g) {
  // Ensure game is idle so Loots will accept OTA
  bcastGameOver(g, /*MANUAL*/2, GAMEOVER_BLAME_ALL);
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

/* ── Setup ────────────────────────────────────────────────── */
void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(BOARD_BLUE_LED, OUTPUT);
  digitalWrite(BOARD_BLUE_LED, LOW);  // off by default

  Serial.println("\n[TREX] Server boot");

  loadRadioConfig();

  OtaCampaign::begin();

  // PIR pins, media, etc. (unchanged)
  for (int i = 0; i < 4; i++) {
    g.pir[i].pin = PIN_PIR[i];
    if (g.pir[i].pin >= 0) {
      pinMode(g.pir[i].pin, INPUT_PULLUP);
      g.pir[i].lastChange = millis();
    }
  }

  mediaInit();
  gameAudioInit();

  TransportConfig cfg{ /*maintenanceMode=*/false, /*wifiChannel=*/WIFI_CHANNEL };
  cfg.txFramed = TX_FRAMED;
  cfg.rxAcceptLegacy = RX_ACCEPT_LEGACY;
  if (!Transport::init(cfg, onRx)) {
    Serial.println("[TREX] Transport init FAILED");
    while (1) delay(1000);
  }
  Serial.printf("Trex header ver: %d\n", TREX_PROTO_VERSION);

  // Game + Mode
  resetGame(g);
  modeClassicInit(g);   // Warmup enabled here
  // Broadcast initial lives to Control/UI
  bcastLivesUpdate(g, /*reason=*/0, GAMEOVER_BLAME_ALL);

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

  // Enter via BOOT long-press or network request
  bool justEntered = Maint::checkRuntimeEntry(mcfg);
  if (!justEntered && netConsumeEnterMaintRequest()) {
    Maint::begin(mcfg);
    justEntered = true;
  }
  if (justEntered || Maint::active) {
    if (!maintLEDOn) { digitalWrite(BOARD_BLUE_LED, HIGH); maintLEDOn = true; }
    Maint::loop();
    return;
  } else if (maintLEDOn) {
    digitalWrite(BOARD_BLUE_LED, LOW);
    maintLEDOn = false;
  }

  // --- Network control commands (from CONTROL station) ---
  if (netConsumeControlStartRequest()) {
    startNewGame(g);
    bcastLivesUpdate(g, /*reason=*/0, GAMEOVER_BLAME_ALL);
  }
  if (netConsumeControlStopRequest()) {
    bcastGameOver(g, /*MANUAL*/2, GAMEOVER_BLAME_ALL);
  }
  if (netConsumeLootOtaRequest()) {
    triggerLootOta(g);
  }

  // --- Radio config requests (from CONTROL station) ---
  RadioCfgPayload rcReq{};
  if (netConsumeRadioCfgRequest(rcReq)) {
    applyRadioCfgAndReboot(rcReq, "CONTROL");
    return;
  }

  OtaCampaign::loop();
  Transport::loop();

  uint32_t now = millis();

  // ---- Serial commands (line-based; keeps 1-char shortcuts) ----
  // Examples:
  //   m            (enter maintenance)
  //   n            (start new game)
  //   u            (loot OTA)
  //   CHAN 11      (move whole game to channel 11, then reboot)
  //   WIRE LEGACY  (txFramed=0 rxLegacy=1)
  //   WIRE FRAMED  (txFramed=1 rxLegacy=1)
  //   WIRE STRICT  (txFramed=1 rxLegacy=0)

  auto handleChar = [&](char c) -> bool {
    if (c=='m' || c=='M') { Maint::begin(mcfg); digitalWrite(BOARD_BLUE_LED, HIGH); return true; }
    if (c=='n' || c=='N') { startNewGame(g); bcastLivesUpdate(g, /*reason=*/0, GAMEOVER_BLAME_ALL); return false; }
    if (c=='g' || c=='G') {
      if (g.roundIndex == 0 || g.phase == Phase::END) {
        startNewGame(g);          // full reset + Round 1 + drip
        bcastLivesUpdate(g, /*reason=*/0, GAMEOVER_BLAME_ALL);
      } else {
        enterGreen(g);            // mid-game manual flip stays supported
      }
      return false;
    }
    if (c=='r' || c=='R') { enterRed(g); return false; }
    if (c=='x' || c=='X') { bcastGameOver(g, /*MANUAL*/2, GAMEOVER_BLAME_ALL); return false; }

    if (c=='u' || c=='U') { triggerLootOta(g); return false; }
    if (c=='b' || c=='B') { bonusForceSpawn(g, millis()); return false; }
    if (c=='c' || c=='C') { bonusClearAll(g); return false; }

    if (c=='1') { modeClassicForceRound(g, 1, /*playWin=*/false); return false; }
    if (c=='2') { modeClassicForceRound(g, 2, /*playWin=*/true ); return false; }
    if (c=='3') { modeClassicForceRound(g, 3, /*playWin=*/true ); return false; }
    if (c=='4') { modeClassicForceRound(g, 4, /*playWin=*/true ); return false; }
    if (c=='>' || c=='.') { modeClassicNextRound(g, /*playWin=*/true); return false; }

    return false;
  };

  static char   lineBuf[96];
  static size_t lineLen = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      lineBuf[lineLen] = 0;
      String line(lineBuf);
      line.trim();
      lineLen = 0;

      if (line.length() == 0) continue;

      // 1-char shortcuts are still supported
      if (line.length() == 1) {
        if (handleChar(line[0])) return; // enter maint: exit loop()
        continue;
      }

      String u = line;
      u.trim();
      u.toUpperCase();

      if (u.startsWith("CHAN ")) {
        int ch = u.substring(5).toInt();
        if (ch >= 1 && ch <= 13) {
          RadioCfgPayload req{};
          req.wifiChannel = (uint8_t)ch;
          req.txFramed    = 255; // keep
          req.rxLegacy    = 255; // keep
          req._pad        = 0;
          applyRadioCfgAndReboot(req, "SERIAL CHAN");
          return;
        } else {
          Serial.println("[RADIO] Usage: CHAN <1..13>");
        }
        continue;
      }

      if (u.startsWith("WIRE ")) {
        String mode = u.substring(5);
        mode.trim();

        RadioCfgPayload req{};
        req.wifiChannel = 0;   // keep
        req._pad        = 0;

        if (mode == "LEGACY") {
          req.txFramed = 0; req.rxLegacy = 1;
          applyRadioCfgAndReboot(req, "SERIAL WIRE LEGACY");
          return;
        }
        if (mode == "FRAMED") {
          req.txFramed = 1; req.rxLegacy = 1;
          applyRadioCfgAndReboot(req, "SERIAL WIRE FRAMED");
          return;
        }
        if (mode == "STRICT") {
          req.txFramed = 1; req.rxLegacy = 0;
          applyRadioCfgAndReboot(req, "SERIAL WIRE STRICT");
          return;
        }

        Serial.println("[RADIO] Usage: WIRE LEGACY | WIRE FRAMED | WIRE STRICT");
        continue;
      }

      if (u == "RADIO" || u == "RADIO?") {
        Serial.printf("[RADIO] Current: chan=%u txFramed=%u rxLegacy=%u\n",
                      (unsigned)WIFI_CHANNEL,
                      (unsigned)(TX_FRAMED ? 1 : 0),
                      (unsigned)(RX_ACCEPT_LEGACY ? 1 : 0));
        continue;
      }

      Serial.println("[SERIAL] Unknown cmd. Try: CHAN <1..13> | WIRE LEGACY/FRAMED/STRICT | RADIO");
      continue;
    }

    if ((c == 8 || c == 127) && lineLen > 0) { lineLen--; continue; }
    if (lineLen < sizeof(lineBuf)-1) lineBuf[lineLen++] = c;
  }


  // Drip broadcast on new game start (unchanged)
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

  // STATE_TICK @ tickHz (only while PLAYING; use current phase timer)
  const uint32_t tickMs = max<uint32_t>(10, 1000 / g.tickHz);
  if (now - g.lastTickSentMs >= tickMs) {
    if (g.phase == Phase::PLAYING) {
      uint32_t msLeft = 0;
      if      (g.bonusIntermission)  msLeft = (g.bonusInterEnd  > now)?(g.bonusInterEnd  - now):0;
      else if (g.bonusIntermission2) msLeft = (g.bonus2End      > now)?(g.bonus2End      - now):0;
      else                           msLeft = (g.roundEndAt     > now)?(g.roundEndAt     - now):0;
      sendStateTick(g, msLeft); 
      bcastGameStatus(g);
    }
    g.lastTickSentMs = now;
  }

  // === Minigame tick ===
  if (g.mgActive) {
    const uint32_t now = millis();

    // Stop by timer
    if ((int32_t)(now - g.mgDeadline) >= 0) {
      g.mgActive = false;
      bcastMgStop(g);
      // Resume normal flow: proceed to Round 5
      modeClassicForceRound(g, 5, /*playWin=*/false);
      return;
    }

    // Or stop 3 s after all stations have tried
    if (g.mgAllTriedAt && (now - g.mgAllTriedAt >= 3000)) {
      g.mgActive = false;
      bcastMgStop(g);
      modeClassicForceRound(g, 5, /*playWin=*/false);
      return;
    }
    
    return;
    // While MG is active, keep running the rest of loop (STATE_TICK, Transport::loop, etc.)
    // but avoid advancing rounds elsewhere; just 'return' after mg handling if you want it isolated.
  }

  // Accrual while GREEN and YELLOW (tick every lootRateMs; grant lootPerTick each tick)
  if (g.phase == Phase::PLAYING &&
    (g.light == LightState::GREEN || g.light == LightState::YELLOW)) {
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

  // Stop any active holds the moment we enter RED (non-punitive).
  static LightState lastLight = LightState::RED;  // pessimistic init
  if (g.phase == Phase::PLAYING) {
    // RED rising edge
    if (g.light == LightState::RED && lastLight != LightState::RED) {
      for (auto &h : g.holds) {
        if (h.active) {
          sendHoldEnd(g, h.holdId, /*RED*/2);
          h.active = false;
        }
      }
    }
    // Safety net: after the configured grace window, make sure no hold lingers
    if (g.light == LightState::RED && now >= g.redGraceUntil) {
      for (auto &h : g.holds) {
        if (h.active) {
          sendHoldEnd(g, h.holdId, /*RED*/2);
          h.active = false;
        }
      }
    }
    lastLight = g.light;
  }

  // PIR violation during RED (after arming delay)
  if (g.phase == Phase::PLAYING && g.light == LightState::RED && g.pirEnforce) {
    if (now >= g.pirArmAt) {
      for (int i = 0; i < 4; ++i) {
        int pin = g.pir[i].pin;
        if (pin >= 0) {
          bool trig = (digitalRead(pin) == LOW);  // active-LOW

          // Track PIR state + edge for debouncing life loss
          const bool prev = g.pir[i].last;
          g.pir[i].state = trig;
          if (trig != prev) {
            g.pir[i].last = trig;
            g.pir[i].lastChange = now;
          }

          // On rising edge after arming delay, consume a life (up to 5)
          if (trig && !prev) {
            // NEW: Only allow ONE life loss per RED period (until the next time we enter RED)
            if (g.pirLifeLostThisRed) {
              continue; // ignore additional PIR trips during this same RED
            }

            const LifeLossResult r = applyLifeLoss(g, /*RED_PIR*/3, GAMEOVER_BLAME_ALL, /*obeyLockout=*/true);
            if (r == LifeLossResult::GAME_OVER) {
              return;
            }

            if (r == LifeLossResult::LIFE_LOST) {
              // Mark that we've already consumed a life for PIR in this RED period.
              g.pirLifeLostThisRed = true;

              // Give the team a recovery window and avoid rapid re-triggering
              enterGreen(g);
              return;
            }

            // IGNORED (lockout): keep running the loop normally.
            continue;
          }
        }
      }
    }
  }

  // Level progression (Classic mode)
  if (g.phase == Phase::PLAYING) {
    modeClassicOnPlayingTick(g, now);
    modeClassicMaybeAdvance(g);
  }

  if      (g.bonusIntermission)  { tickBonusIntermission(g, now); }
  else if (g.bonusIntermission2) { tickBonusIntermission2(g, now); }
  else                           { tickCadence(g, now); }

}
