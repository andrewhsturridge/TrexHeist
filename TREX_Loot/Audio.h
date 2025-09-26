#pragma once
#include <stdint.h>

// NOTE: This header exposes exactly what the Loot sketch already uses.
// Keep your existing I2S bring-up in setup(); it will assign to i2sOut.

// Forward decls to avoid heavy includes in the header:
class AudioOutputI2S;
class AudioGeneratorWAV;

// Externals that Loot uses/sets
extern AudioOutputI2S* i2sOut;      // created in setup() in TREX_Loot.ino
extern AudioGeneratorWAV* decoder;   // owned by audio module, but exposed for parity
extern bool playing;                 // true while a clip is running

// Flags read by Loot logic on HOLD_END/tag removal & rainbow cadence
extern bool     g_audioOneShot;          // true => one-shot; don't auto-restart & don't auto-stop on HOLD_END
extern bool     g_chimeActive;           // true while spawn chime plays (pre-empts loop)
extern uint32_t g_bonusExclusiveUntilMs; // short gentle window after bonus start

// Loot’s audio API (names preserved)
bool startAudio();
void stopAudio();
void handleAudio();                 // call regularly while playing
bool startLootAudio(bool bonus);    // select loop/bonus clip and start (sets one-shot + exclusive window)
void playBonusSpawnChime();         // pre-empts loop and plays spawn chime fully
void scheduleAudioStop(uint16_t delayMs);
void tickScheduledAudio();

// The Loot sketch uses holdActive in handleAudio()’s resume path:
extern volatile bool holdActive;    // defined in TREX_Loot.ino
