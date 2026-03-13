#include "GameModel.h"
#include <string.h>
#include <TrexProtocol.h> 

#include "ModeClassic.h"

static inline bool uidEq(const TrexUid& a, const TrexUid& b) {
  if (a.len != b.len) return false;
  for (uint8_t i=0;i<a.len;i++) if (a.bytes[i]!=b.bytes[i]) return false;
  return true;
}

#include <TrexProtocol.h>  // for TrexUid helpers if used
#include "GameModel.h"

void resetGame(Game& g) {
  g.phase = Phase::PLAYING;       // ensure we're in play mode
  g.teamScore = 0;

  // Reset sequence / drip broadcast scheduler
  g.seq = 1;
  g.pending = PendingStart{};
  g.lastTickSentMs = 0;

  // Lives reset
  g.livesMax             = 5;
  g.livesRemaining       = g.livesMax;
  g.lifeLossCooldownMs   = 1500;
  g.lifeLossLockoutUntil = 0;
  g.lastLifeLossReason   = 0;
  g.lastLifeLossBlameSid = GAMEOVER_BLAME_ALL;
  g.pirLifeLostThisRed   = false;
  g.roundIndex = 0;

  g.gameStartAt  = 0;
  g.gameEndAt    = 0;
  g.roundStartAt = 0;
  g.roundEndAt   = 0;
  g.roundStartScore = 0;

  // Clear all stage/auxiliary modes
  g.bonusIntermission   = false;
  g.bonusIntermission2  = false;
  g.bonusInterStart     = 0;
  g.bonusInterEnd       = 0;
  g.bonusWarnTickStarted= false;

  g.bonus2Start     = 0;
  g.bonus2End       = 0;
  g.bonus2Sid       = 0;
  g.bonus2NextHopAt = 0;
  g.bonus2Idx       = 0;

  g.mgActive        = false;
  g.mgStartedAt     = 0;
  g.mgDeadline      = 0;
  g.mgAllTriedAt    = 0;
  g.mgTriedMask     = 0;
  g.mgSuccessMask   = 0;
  g.mgExpectedStations = MAX_STATIONS;
  g.mgCfg           = Game::MgConfig{};

  g.r5Active        = false;
  g.r5HotSid        = 0;
  g.r5DwellEndAt    = 0;
  g.r5NextDepleteAt = 0;

  g.light = LightState::GREEN;
  g.nextSwitch = 0;
  g.lastFlipMs = 0;
  g.redGraceUntil = 0;
  g.pirArmAt = 0;

  g.noRedThisRound       = false;
  g.allowYellowThisRound = true;

  g.lootPerTick = 1;
  g.lootRateMs  = 1000;

  // Bonus reset
  g.bonusActiveMask = 0;
  // Index 0 is unused; clear 0..MAX_STATIONS for safety.
  for (int i = 0; i <= MAX_STATIONS; ++i) g.bonusEndsAt[i] = 0;
  g.bonusNextSpawnAt = 0;
  g.bonusSpawnsThisRound = 0;

  // Clear station state; Round 1 will set inventory=20 later
  for (uint8_t sid = 1; sid <= MAX_STATIONS; ++sid) {
    g.stationCapacity[sid]  = 56;  // keep your physical gauge size
    g.stationInventory[sid] = 0;   // start empty; filled in startRound()
  }

  g.greenMsMin = g.greenMsMax = 0;
  g.redMsMin   = g.redMsMax   = 0;
  g.yellowMsMin= g.yellowMsMax= 0;

  // (Optional) clear any per-player holds/carry if you track them here
  for (int i=0;i<MAX_PLAYERS;i++) {
    g.players[i] = PlayerRec{}; // zero/clear
  }

  // Clear any holds explicitly (safety)
  for (int i=0;i<MAX_HOLDS;i++) {
    g.holds[i] = HoldRec{};
  }
}

int findPlayer(const Game& g, const TrexUid& u) {
  for (int i=0;i<MAX_PLAYERS;i++) if (g.players[i].used && uidEq(g.players[i].uid,u)) return i;
  return -1;
}
int ensurePlayer(Game& g, const TrexUid& u) {
  int idx = findPlayer(g,u);
  if (idx>=0) return idx;
  for (int i=0;i<MAX_PLAYERS;i++) if (!g.players[i].used) {
    g.players[i].used=true; g.players[i].uid=u; g.players[i].carried=0; g.players[i].banked=0;
    return i;
  }
  return -1;
}
int findHoldById(const Game& g, uint32_t hid) {
  for (int i=0;i<MAX_HOLDS;i++) if (g.holds[i].active && g.holds[i].holdId==hid) return i;
  return -1;
}
int allocHold(Game& g) {
  for (int i=0;i<MAX_HOLDS;i++) if (!g.holds[i].active) return i;
  return -1;
}

void startNewGame(Game& g) {
  Serial.println("[TREX] New game starting...");
  resetGame(g);
  modeClassicInit(g);
}
