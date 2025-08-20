#include "GameModel.h"
#include <string.h>

static inline bool uidEq(const TrexUid& a, const TrexUid& b) {
  if (a.len != b.len) return false;
  for (uint8_t i=0;i<a.len;i++) if (a.bytes[i]!=b.bytes[i]) return false;
  return true;
}

void resetGame(Game& g) {
  g.phase = Phase::PLAYING;
  g.light = LightState::GREEN;
  g.nextSwitch = 0;
  g.seq = 1;
  g.teamScore = 0;

  for (int i=0;i<MAX_PLAYERS;i++) { g.players[i].used=false; g.players[i].carried=0; g.players[i].banked=0; }
  for (int i=0;i<MAX_HOLDS;i++) g.holds[i].active=false;

  // inventory reset to capacity
  for (uint8_t sid=1; sid<=5; ++sid) g.stationInventory[sid]=g.stationCapacity[sid];

  g.pending.needGameStart = true;
  g.pending.nextStation   = 1;
  g.pending.needScore     = true;

  g.warmupActive = true;
  g.warmupEndAt  = millis() + g.warmupMs;
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
}
