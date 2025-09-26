#include "LootRx.h"
#include <Arduino.h>
#include <TrexProtocol.h>

#include "Audio.h"      // startLootAudio, stopAudio, playing, g_audioOneShot, g_chimeActive
#include "LootLeds.h"   // fillRing/fillGauge/blinkers/drawGaugeAuto/canPaintGaugeNow
#include "Identity.h"   // STATION_ID

// -------- fallbacks for cross-TU constants (optional: move to a shared header) ----------
#ifndef PIN_MOSFET
#define PIN_MOSFET 17
#endif
#ifndef AUDIO_STOP_STAGGER_MS
#define AUDIO_STOP_STAGGER_MS 12
#endif

// -------- externs provided elsewhere (unchanged names) --------
// Identity / config
extern uint8_t STATION_ID;

// Game/light state
extern volatile bool gameActive;
extern volatile bool holdActive;
extern bool          wasPaused;
extern bool          stationInited;
extern bool          otaInProgress;
extern bool          tagPresent;
extern bool          fullBlinkActive;
extern bool          fullAnnounced;
extern bool          gaugeCacheValid;
extern bool          s_isBonusNow;
extern bool          g_bonusAtTap;
extern LightState    g_lightState;

// Inventory/capacity and carry
extern uint16_t inv, cap;
extern uint8_t  carried, maxCarry;
extern uint32_t holdId;

// UI / timers
extern bool     yellowBlinkActive;
extern bool     yellowBlinkOn;
extern uint32_t nextGaugeDrawAtMs;

// OTA config-update latches
extern bool      otaStartRequested;
extern bool      otaInProgress;
extern uint32_t  otaCampaignId;
extern uint8_t   otaExpectMajor, otaExpectMinor;
extern char      otaUrl[128];

// Visual helpers
extern void stopYellowBlink();
extern void startEmptyBlink();
extern void stopEmptyBlink();
extern void startFullBlinkImmediate();
extern void stopFullBlink();
extern void gameOverBlinkAndOff();
extern void drawRingCarried(uint8_t cur, uint8_t maxC);

// Audio helpers
extern void playBonusSpawnChime();
extern void scheduleAudioStop(uint16_t delayMs);

// ------------------ implementation ------------------

void onRx(const uint8_t* data, uint16_t len) {
  if (len < sizeof(MsgHeader)) return;
  auto* h = (const MsgHeader*)data;
  if (h->version != TREX_PROTO_VERSION) {
    Serial.printf("[WARN] Proto mismatch on RX: got=%u exp=%u (type=%u)\n",
                  h->version, (unsigned)TREX_PROTO_VERSION, h->type);
    return;
  }

  switch ((MsgType)h->type) {
    case MsgType::STATE_TICK: {
      if (h->payloadLen < 1) break;
      const StateTickPayload* p =
          (const StateTickPayload*)(data + sizeof(MsgHeader));

      // Update light state
      if      (p->state == (uint8_t)LightState::GREEN)  g_lightState = LightState::GREEN;
      else if (p->state == (uint8_t)LightState::YELLOW) g_lightState = LightState::YELLOW;
      else                                              g_lightState = LightState::RED;

      // Arm/stop Yellow blink
      if (g_lightState == LightState::YELLOW) yellowBlinkActive = true;
      else                                    stopYellowBlink();

      // After GAME_OVER keep gauge off; in steady RED show ring if idle
      if (!gameActive) {
        if (g_lightState == LightState::RED && !holdActive && !otaInProgress) fillRing(0x00FF0000 /*RED*/);
        break;
      }

      // During game: draw gauge unless Yellow OFF phase
      if (stationInited && canPaintGaugeNow()) {
        drawGaugeAuto(inv, cap);
      }
      break;
    }

    case MsgType::LOOT_HOLD_ACK: {
      if (h->payloadLen != sizeof(LootHoldAckPayload)) break;
      const auto* p = (const LootHoldAckPayload*)(data + sizeof(MsgHeader));
      if (p->holdId != holdId) break;

      // Update state from ACK
      maxCarry = p->maxCarry;
      carried  = (p->carried > maxCarry) ? maxCarry : p->carried;  // clamp
      inv      = p->inventory;
      cap      = p->capacity;
      stationInited = true;

      // Maintain empty indicator
      if (tagPresent && inv == 0) startEmptyBlink();
      else                        stopEmptyBlink();

      if (p->accepted) {
        if (!gameActive) break;   // ignore visuals if game ended mid-flight
        holdActive = true;

        // === forceStartLootAudio(g_bonusAtTap || s_isBonusNow) ===
        // Pre-empt any chime/loop and start the correct clip immediately.
        const bool wantBonus = (g_bonusAtTap || s_isBonusNow);
        if (playing) stopAudio();
        g_chimeActive = false;
        startLootAudio(wantBonus);

        // FULL indicator (ring blink) vs carried ring
        if (carried >= maxCarry) {
          if (!fullAnnounced || blinkHoldId != p->holdId) {
            startFullBlinkImmediate();
            fullAnnounced = true;
            blinkHoldId   = p->holdId;
            // Guard: don’t stop if it’s a bonus one-shot or a chime is active
            if (!(s_isBonusNow || g_audioOneShot || g_chimeActive)) {
              scheduleAudioStop(AUDIO_STOP_STAGGER_MS);
            }
          }
        } else {
          if (fullBlinkActive) stopFullBlink();
          fullAnnounced = false;
          drawRingCarried(carried, maxCarry);
        }

        // Throttled repaint first so LED frame doesn’t coincide with audio begin
        uint32_t now = millis();
        if ((int32_t)(now - nextGaugeDrawAtMs) >= 0 && canPaintGaugeNow()) {
          drawGaugeAuto(inv, cap);
          nextGaugeDrawAtMs = now + 20;
        }
      } else {
        // Denied (FULL/EMPTY/EDGE_GRACE/RED etc.)
        holdActive = false;

        if (carried >= maxCarry) {
          // FULL → start blink; stop audio via scheduled stagger
          if (!fullAnnounced || blinkHoldId != p->holdId) {
            startFullBlinkImmediate();
            fullAnnounced = true;
            blinkHoldId   = p->holdId;
            scheduleAudioStop(AUDIO_STOP_STAGGER_MS);
          }
          // Defer gauge repaint; normal updates will paint soon
        } else {
          fullAnnounced = false;
          stopFullBlink();
          fillRing(0x00FF0000 /*RED*/);

          // Reflect inventory now (honors Yellow OFF phase)
          uint32_t now = millis();
          if (gameActive && (int32_t)(now - nextGaugeDrawAtMs) >= 0 && canPaintGaugeNow()) {
            drawGaugeAuto(inv, cap);
            nextGaugeDrawAtMs = now + 20;
          }
        }
      }
      break;
    }

    case MsgType::LOOT_TICK: {
      if (h->payloadLen != sizeof(LootTickPayload)) break;
      const auto* p = (const LootTickPayload*)(data + sizeof(MsgHeader));
      if (!holdActive || p->holdId != holdId) break;

      carried = (p->carried > maxCarry) ? maxCarry : p->carried;  // clamp
      inv     = p->inventory;
      stationInited = true;

      // Maintain empty indicator
      if (tagPresent && inv == 0) startEmptyBlink();
      else                        stopEmptyBlink();

      // FULL handling + ring update
      if (carried >= maxCarry) {
        if (!fullAnnounced || blinkHoldId != p->holdId) {
          startFullBlinkImmediate();
          fullAnnounced = true;
          blinkHoldId   = p->holdId;
          scheduleAudioStop(AUDIO_STOP_STAGGER_MS);
        }
      } else {
        if (fullBlinkActive) stopFullBlink();
        fullAnnounced = false;
        drawRingCarried(carried, maxCarry);
      }

      // Throttled gauge render — GREEN shows rainbow automatically, YELLOW/RED override
      uint32_t now = millis();
      if ((int32_t)(now - nextGaugeDrawAtMs) >= 0 && canPaintGaugeNow()) {
        drawGaugeAuto(inv, cap);
        nextGaugeDrawAtMs = now + 20;
      }
      break;
    }

    case MsgType::HOLD_END: {
      if (h->payloadLen != sizeof(HoldEndPayload)) break;
      const auto* p = (const HoldEndPayload*)(data + sizeof(MsgHeader));
      if (p->holdId != holdId) break;

      stopEmptyBlink();

      holdActive = false;
      holdId     = 0;

      // Do NOT stop audio if the spawn chime is playing
      if (!g_audioOneShot && !g_chimeActive) {
        stopAudio();
      }
      g_bonusAtTap = false;  // clear snapshot at end of hold

      fullAnnounced = false;

      if (tagPresent && carried >= maxCarry) {
        if (!fullBlinkActive) startFullBlinkImmediate();
      } else {
        stopFullBlink();
        fillRing(0x00FF0000 /*RED*/);
      }
      break;
    }

    case MsgType::STATION_UPDATE: {
      if (h->payloadLen != sizeof(StationUpdatePayload)) break;
      const auto* p = (const StationUpdatePayload*)(data + sizeof(MsgHeader));
      if (p->stationId != STATION_ID) break;

      // Cache latest values
      inv = p->inventory;
      cap = p->capacity;
      stationInited = true;

      // Don't repaint after GAME_OVER
      if (!gameActive) break;

      if (!holdActive && !otaInProgress && canPaintGaugeNow()) {
        drawGaugeAuto(inv, cap);
      }
      break;
    }

    case MsgType::GAME_START: {
      gameActive       = true;
      wasPaused        = false;
      fullBlinkActive  = false;
      fullAnnounced    = false;

      stationInited    = false;      // wait for fresh inventory before painting
      gaugeCacheValid  = false;

      digitalWrite(PIN_MOSFET, HIGH);  // lamp ON for live round
      stopFullBlink();
      stopEmptyBlink();
      fillRing(0x00FF0000 /*RED*/);    // ring “awake” in RED

      // Do NOT draw gauge here; wait for STATION_UPDATE
      Serial.println("[LOOT] GAME_START");
      break;
    }

    case MsgType::GAME_OVER: {
      if (len < sizeof(MsgHeader) + 1) break;

      const GameOverPayload* gp =
          (const GameOverPayload*)(data + sizeof(MsgHeader));

      const uint8_t reason   = (h->payloadLen >= 1) ? gp->reason : 0;
      const uint8_t blameSid = (h->payloadLen >= sizeof(GameOverPayload))
                              ? gp->blameSid : GAMEOVER_BLAME_ALL;

      // Force safe post-game condition
      gameActive      = false;
      holdActive      = false;
      tagPresent      = false;
      fullBlinkActive = false;

      // Leave bonus
      s_isBonusNow = false;

      // Lock palette to RED; stop local blinkers & audio
      g_lightState = LightState::RED;
      stopYellowBlink();
      stopEmptyBlink();
      stopAudio();

      // Gauge OFF immediately; ring steady RED (targeted blink still allowed)
      fillGauge(0);
      if (!otaInProgress) fillRing(0x00FF0000 /*RED*/);

      // Offender-based blink (RED_LOOT)
      const bool redViolation = (reason == 1);
      const bool offender     = redViolation &&
                                (blameSid != GAMEOVER_BLAME_ALL) &&
                                (blameSid == STATION_ID);
      const bool shouldBlink  = !redViolation || offender;
      if (shouldBlink) gameOverBlinkAndOff();

      Serial.printf("[LOOT] GAME_OVER reason=%u blame=%u me=%u\n",
                    reason, blameSid, STATION_ID);
      break;
    }

    case MsgType::CONFIG_UPDATE: {
      if (h->payloadLen != sizeof(ConfigUpdatePayload)) break;
      if (gameActive) { Serial.println("[OTA] Ignored (game active)"); break; }
      const auto* p = (const ConfigUpdatePayload*)(data + sizeof(MsgHeader));

      // scope: LOOT + my id (or broadcast)
      bool typeMatch = (p->stationType == 0) || (p->stationType == (uint8_t)StationType::LOOT);
      bool idMatch   = (p->targetId == 0)    || (p->targetId == STATION_ID);
      if (!typeMatch || !idMatch) break;

      if (otaInProgress) { Serial.println("[OTA] Already in progress"); break; }
      if (p->otaUrl[0] == 0) { Serial.println("[OTA] No URL"); break; }

      // latch
      strncpy(otaUrl, p->otaUrl, sizeof(otaUrl)-1); otaUrl[sizeof(otaUrl)-1]=0;
      otaCampaignId  = p->campaignId;
      otaExpectMajor = p->expectMajor; otaExpectMinor = p->expectMinor;
      otaInProgress  = true;
      otaStartRequested = true;

      Serial.printf("[OTA] CONFIG_UPDATE received, url=%s campaign=%lu\n",
                    otaUrl, (unsigned long)otaCampaignId);
      break;
    }

    case MsgType::BONUS_UPDATE: {
      if (h->payloadLen < 4) break;
      const uint8_t* pl = data + sizeof(MsgHeader);
      uint32_t mask = (uint32_t)pl[0]
                    | ((uint32_t)pl[1] << 8)
                    | ((uint32_t)pl[2] << 16)
                    | ((uint32_t)pl[3] << 24);

      // --- Normal BONUS behavior (no R4.5 flag) ---
      bool wasBonus = s_isBonusNow;
      s_isBonusNow  = ((mask >> STATION_ID) & 0x1u) != 0;

      if (!wasBonus && s_isBonusNow) {
        // Entering bonus: chime + kill blinkers so nothing blanks the next frame
        playBonusSpawnChime();
        stopYellowBlink();
        stopEmptyBlink();
      }

      // Paint immediately, bypassing yellow OFF gating so the frame can’t be blanked
      if (gameActive && stationInited && !otaInProgress) {
        gaugeCacheValid = false;
        drawGaugeAuto(inv, cap);   // draws rainbow in GREEN when s_isBonusNow==true
      }
      break;
    }

    default:
      break;
  }
}
