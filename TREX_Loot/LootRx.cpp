#include "LootRx.h"
#include <Arduino.h>
#include <TrexProtocol.h>

#include "Audio.h"
#include "LootLeds.h"
#include "LootNet.h"
#include "LootMini.h"
#include "Identity.h"

#ifndef PIN_MOSFET
#define PIN_MOSFET 17
#endif
#ifndef AUDIO_STOP_STAGGER_MS
#define AUDIO_STOP_STAGGER_MS 12
#endif

// -------- externs --------
extern uint8_t STATION_ID;

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
extern bool          g_bonusAtTap;    // lives in the .ino; cleared on HOLD_END
extern LightState    g_lightState;

extern uint16_t inv, cap;
extern uint8_t  carried, maxCarry;
extern uint32_t holdId;

extern bool     yellowBlinkActive;
extern bool     yellowBlinkOn;
extern uint32_t nextGaugeDrawAtMs;

extern bool      otaStartRequested;
extern bool      otaInProgress;
extern uint32_t  otaCampaignId;
extern uint8_t   otaExpectMajor, otaExpectMinor;
extern char      otaUrl[128];

extern void stopYellowBlink();
extern void startEmptyBlink();
extern void stopEmptyBlink();
extern void startFullBlinkImmediate();
extern void stopFullBlink();
extern void gameOverBlinkAndOff();
extern void drawRingCarried(uint8_t cur, uint8_t maxC);

extern void playBonusSpawnChime();
extern void scheduleAudioStop(uint16_t delayMs);

// mg firewall
extern volatile bool mgActive;
static inline bool mgSwallowRepaints() { return mgActive && !otaInProgress; }

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

      if      (p->state == (uint8_t)LightState::GREEN)  g_lightState = LightState::GREEN;
      else if (p->state == (uint8_t)LightState::YELLOW) g_lightState = LightState::YELLOW;
      else                                              g_lightState = LightState::RED;

      if (g_lightState == LightState::YELLOW) yellowBlinkActive = true;
      else                                    stopYellowBlink();

      if (!gameActive) {
        if (g_lightState == LightState::RED && !holdActive && !otaInProgress) fillRing(Adafruit_NeoPixel::Color(255,0,0));
        break;
      }

      if (mgSwallowRepaints()) break;
      if (stationInited && canPaintGaugeNow()) drawGaugeAuto(inv, cap);
      break;
    }

    case MsgType::LOOT_HOLD_ACK: {
      if (mgActive) break;
      if (h->payloadLen != sizeof(LootHoldAckPayload)) break;
      const auto* p = (const LootHoldAckPayload*)(data + sizeof(MsgHeader));
      if (p->holdId != holdId) break;

      maxCarry = p->maxCarry;
      carried  = (p->carried > maxCarry) ? maxCarry : p->carried;
      inv      = p->inventory;
      cap      = p->capacity;
      stationInited = true;

      if (tagPresent && inv == 0) startEmptyBlink();
      else                        stopEmptyBlink();

      if (p->accepted) {
        if (!gameActive) break;
        holdActive = true;

        const bool wantBonus = (g_bonusAtTap || s_isBonusNow);
        if (playing) stopAudio();
        g_chimeActive = false;
        startLootAudio(wantBonus);

        if (carried >= maxCarry) {
          if (!fullAnnounced || blinkHoldId != p->holdId) {
            startFullBlinkImmediate();
            fullAnnounced = true;
            blinkHoldId   = p->holdId;
            if (!(s_isBonusNow || g_audioOneShot || g_chimeActive)) scheduleAudioStop(AUDIO_STOP_STAGGER_MS);
          }
        } else {
          if (fullBlinkActive) stopFullBlink();
          fullAnnounced = false;
          drawRingCarried(carried, maxCarry);
        }

        uint32_t now = millis();
        if ((int32_t)(now - nextGaugeDrawAtMs) >= 0 && canPaintGaugeNow()) {
          drawGaugeAuto(inv, cap);
          nextGaugeDrawAtMs = now + 20;
        }
      } else {
        holdActive = false;

        if (carried >= maxCarry) {
          if (!fullAnnounced || blinkHoldId != p->holdId) {
            startFullBlinkImmediate();
            fullAnnounced = true;
            blinkHoldId   = p->holdId;
            scheduleAudioStop(AUDIO_STOP_STAGGER_MS);
          }
        } else {
          fullAnnounced = false;
          stopFullBlink();
          fillRing(Adafruit_NeoPixel::Color(255,0,0));
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
      if (mgActive) break;
      if (h->payloadLen != sizeof(LootTickPayload)) break;
      const auto* p = (const LootTickPayload*)(data + sizeof(MsgHeader));
      if (!holdActive || p->holdId != holdId) break;

      carried = (p->carried > maxCarry) ? maxCarry : p->carried;
      inv     = p->inventory;
      stationInited = true;

      if (tagPresent && inv == 0) startEmptyBlink();
      else                        stopEmptyBlink();

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

      uint32_t now = millis();
      if ((int32_t)(now - nextGaugeDrawAtMs) >= 0 && canPaintGaugeNow()) {
        drawGaugeAuto(inv, cap);
        nextGaugeDrawAtMs = now + 20;
      }
      break;
    }

    case MsgType::HOLD_END: {
      if (mgActive) break;
      if (h->payloadLen != sizeof(HoldEndPayload)) break;
      const auto* p = (const HoldEndPayload*)(data + sizeof(MsgHeader));
      if (p->holdId != holdId) break;

      stopEmptyBlink();

      holdActive = false;
      holdId     = 0;

      if (!g_audioOneShot && !g_chimeActive) stopAudio();
      g_bonusAtTap = false;

      fullAnnounced = false;

      if (tagPresent && carried >= maxCarry) {
        if (!fullBlinkActive) startFullBlinkImmediate();
      } else {
        stopFullBlink();
        fillRing(Adafruit_NeoPixel::Color(255,0,0));
      }
      break;
    }

    case MsgType::STATION_UPDATE: {
      const auto* p = (const StationUpdatePayload*)(data + sizeof(MsgHeader));
      if (h->payloadLen != sizeof(StationUpdatePayload)) break;
      if (p->stationId != STATION_ID) break;

      inv = p->inventory;
      cap = p->capacity;
      stationInited = true;

      if (!gameActive) break;
      if (mgSwallowRepaints()) break;

      if (!holdActive && !otaInProgress && canPaintGaugeNow()) {
        drawGaugeAuto(inv, cap);
      }
      break;
    }

    case MsgType::GAME_START: {
      mgCancel();                     // cancel minigame immediately
      gameActive       = true;
      wasPaused        = false;
      fullBlinkActive  = false;
      fullAnnounced    = false;

      stationInited    = false;
      gaugeCacheValid  = false;

      digitalWrite(PIN_MOSFET, HIGH);
      stopFullBlink();
      stopEmptyBlink();
      fillRing(Adafruit_NeoPixel::Color(255,0,0));
      Serial.println("[LOOT] GAME_START");
      break;
    }

    case MsgType::GAME_OVER: {
      mgCancel();                     // cancel minigame immediately
      if (len < sizeof(MsgHeader) + 1) break;
      const GameOverPayload* gp =
          (const GameOverPayload*)(data + sizeof(MsgHeader));

      const uint8_t reason   = (h->payloadLen >= 1) ? gp->reason : 0;
      const uint8_t blameSid = (h->payloadLen >= sizeof(GameOverPayload))
                              ? gp->blameSid : GAMEOVER_BLAME_ALL;

      gameActive      = false;
      holdActive      = false;
      tagPresent      = false;
      fullBlinkActive = false;

      s_isBonusNow = false;
      g_lightState = LightState::RED;
      stopYellowBlink();
      stopEmptyBlink();
      stopAudio();

      fillGauge(0);
      if (!otaInProgress) fillRing(Adafruit_NeoPixel::Color(255,0,0));

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
      mgCancel();                     // OTA takes priority; cancel MG
      if (h->payloadLen != sizeof(ConfigUpdatePayload)) break;
      if (gameActive) { Serial.println("[OTA] Ignored (game active)"); break; }
      const auto* p = (const ConfigUpdatePayload*)(data + sizeof(MsgHeader));

      bool typeMatch = (p->stationType == 0) || (p->stationType == (uint8_t)StationType::LOOT);
      bool idMatch   = (p->targetId == 0)    || (p->targetId == STATION_ID);
      if (!typeMatch || !idMatch) break;

      if (otaInProgress) { Serial.println("[OTA] Already in progress"); break; }
      if (p->otaUrl[0] == 0) { Serial.println("[OTA] No URL"); break; }

      strncpy(otaUrl, p->otaUrl, sizeof(otaUrl)-1); otaUrl[sizeof(otaUrl)-1]=0;
      otaCampaignId  = p->campaignId;
      otaExpectMajor = p->expectMajor; otaExpectMinor = p->expectMinor;
      otaInProgress  = true;
      otaStartRequested = true;

      Serial.printf("[OTA] CONFIG_UPDATE received, url=%s campaign=%lu\n",
                    otaUrl, (unsigned long)otaCampaignId);
      break;
    }

    // ---- Minigame messages ----
    case MsgType::MG_START: {
      if (h->payloadLen != sizeof(MgStartPayload)) break;
      const auto* p = (const MgStartPayload*)(data + sizeof(MsgHeader));

      MgParams mp;
      mp.seed       = p->seed;
      mp.timerMs    = p->timerMs;
      mp.speedMinMs = p->speedMinMs;
      mp.speedMaxMs = p->speedMaxMs;
      mp.segMin     = p->segMin;
      mp.segMax     = p->segMax;
      mgStart(mp);
      break;
    }

    case MsgType::MG_STOP: {
      mgStop();
      break;
    }

    case MsgType::BONUS_UPDATE: {
      const uint8_t* pl = data + sizeof(MsgHeader);
      if (h->payloadLen < 4) break;
      uint32_t mask = (uint32_t)pl[0]
                    | ((uint32_t)pl[1] << 8)
                    | ((uint32_t)pl[2] << 16)
                    | ((uint32_t)pl[3] << 24);

      bool wasBonus = s_isBonusNow;
      s_isBonusNow  = ((mask >> STATION_ID) & 0x1u) != 0;

      if (mgActive) break; // swallow chime/paint during minigame

      if (!wasBonus && s_isBonusNow) {
        playBonusSpawnChime();
        stopYellowBlink();
        stopEmptyBlink();
      }

      if (gameActive && stationInited && !otaInProgress) {
        gaugeCacheValid = false;
        drawGaugeAuto(inv, cap);
      }
      break;
    }

    default:
      break;
  }
}
