#include "ModeClassic.h"

struct LevelCfg {
  uint32_t green_ms, red_ms, loot_ms;
  uint8_t  max_carry;
  uint16_t score_to_advance; // 0 => terminal level
};

static const LevelCfg kLevels[] = {
  /* L1 */ { 10000, 8000, 1000, 8,  20 },
  /* L2 */ { 5500, 4500,  900, 8,  50 },
  /* L3 */ { 5000, 5000,  800, 7,  90 },
  /* L4 */ { 4500, 5500,  700, 6,   0 }, // terminal for now
};

static void applyLevel(Game& g, const LevelCfg& L) {
  g.greenMs   = L.green_ms;
  g.redMs     = L.red_ms;
  g.lootRateMs= L.loot_ms;
  g.maxCarry  = L.max_carry;
  Serial.printf("[TREX] Apply Level %u  G=%u R=%u loot=%u maxCarry=%u\n",
                (unsigned)(g.levelIndex+1), (unsigned)L.green_ms, (unsigned)L.red_ms,
                (unsigned)L.loot_ms, (unsigned)L.max_carry);
}

void modeClassicInit(Game& g) {
  // Warmup: GREEN-only window; PIR ignored by server logic while warmupActive=true
  g.warmupActive = true;
  g.warmupMs     = 30000; // change via maint if desired
  g.warmupEndAt  = millis() + g.warmupMs;
  g.levelIndex   = 0;     // next real level is L1
}

void modeClassicMaybeAdvance(Game& g) {
  if (g.levelIndex >= (sizeof(kLevels)/sizeof(kLevels[0]))) return;

  const LevelCfg& cur = kLevels[g.levelIndex];

  // When entering level (first time after warmup or after prior advancement),
  // ensure tunables match the table.
  static uint8_t lastAppliedLevel = 255;
  if (lastAppliedLevel != g.levelIndex) {
    applyLevel(g, cur);
    lastAppliedLevel = g.levelIndex;
  }

  // Advance once score threshold reached
  if (cur.score_to_advance > 0 && g.teamScore >= cur.score_to_advance) {
    g.levelIndex++;
    if (g.levelIndex < (sizeof(kLevels)/sizeof(kLevels[0]))) {
      const LevelCfg& nxt = kLevels[g.levelIndex];
      applyLevel(g, nxt);
    }
  }
}
