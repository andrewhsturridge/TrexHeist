#include "Audio.h"
#include <Arduino.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>

// If you prefer, move TREX_AUDIO_PROGMEM into a shared header.
// We default to PROGMEM when not provided (keeps current behavior).
#ifndef TREX_AUDIO_PROGMEM
#define TREX_AUDIO_PROGMEM 1
#warning "TREX_AUDIO_PROGMEM not defined for Audio.cpp; defaulting to 1 (PROGMEM). Move the macro to a shared header if you plan to toggle FS vs PROGMEM."
#endif

#if TREX_AUDIO_PROGMEM
  #include <AudioFileSourcePROGMEM.h>
  #include "replenish.h"
  #include "replenish_bonus.h"
  #include "bonus_spawn.h"   // spawn chime on BONUS rising edge

  // Selected clip pointers (normal replenish by default)
  static const uint8_t* g_clipData = replenish_wav;
  static size_t         g_clipLen  = replenish_wav_len;

  // In-place storage to avoid heap fragmentation
  alignas(4) static uint8_t wavSrcStorage[sizeof(AudioFileSourcePROGMEM)];
  static AudioFileSourcePROGMEM* wavSrc = nullptr;
#else
  #include <LittleFS.h>
  #include <AudioFileSourceLittleFS.h>
  #include <AudioFileSourceBuffer.h>
  // FS clip paths (mirrors your sketch)
  static constexpr char CLIP_PATH[]        = "/replenish.wav";
  static constexpr char CLIP_PATH_BONUS[]  = "/replenish_bonus.wav";
  static constexpr char CLIP_PATH_SPAWN[]  = "/bonus_spawn.wav";
  static const char* g_clipPath = CLIP_PATH;

  static AudioFileSourceLittleFS* wavFile = nullptr;
  static AudioFileSourceBuffer*   wavBuf  = nullptr;
#endif

// Externals (definitions here)
AudioOutputI2S*   i2sOut  = nullptr;
AudioGeneratorWAV* decoder = nullptr;
bool               playing = false;

// Flags used by Loot logic (definitions here)
bool     g_audioOneShot = false;
bool     g_chimeActive  = false;
uint32_t g_bonusExclusiveUntilMs = 0;

// Internal: whether to resume replenish loop after spawn chime (only if hold still active)
static bool g_resumeLoopAfterChime = false;

// From Loot sketch
extern volatile bool holdActive;

// ---- internals --------------------------------------------------------------

static bool openChain() {
  if (decoder && decoder->isRunning()) decoder->stop();

#if TREX_AUDIO_PROGMEM
  // Use currently selected PROGMEM clip
  wavSrc = new (wavSrcStorage) AudioFileSourcePROGMEM(g_clipData, g_clipLen);
#else
  // Use currently selected LittleFS path
  if (wavBuf)  { delete wavBuf;  wavBuf  = nullptr; }
  if (wavFile) { delete wavFile; wavFile = nullptr; }
  wavFile = new AudioFileSourceLittleFS(g_clipPath);
  if (!wavFile) { Serial.println("[LOOT] wavFile alloc fail"); return false; }
  wavBuf = new AudioFileSourceBuffer(wavFile, 4096);
  if (!wavBuf)  { Serial.println("[LOOT] wavBuf alloc fail");  return false; }
#endif

  // Fresh decoder each time avoids stale state
  if (decoder) { decoder->stop(); delete decoder; decoder = nullptr; }
  decoder = new AudioGeneratorWAV();

#if TREX_AUDIO_PROGMEM
  bool ok = decoder->begin(wavSrc, i2sOut);
#else
  bool ok = decoder->begin(wavBuf, i2sOut);
#endif
  if (!ok) Serial.println("[LOOT] decoder.begin() failed");
  return ok;
}

static inline void selectClip(bool bonus) {
#if TREX_AUDIO_PROGMEM
  if (bonus) { g_clipData = replenish_bonus_wav; g_clipLen = replenish_bonus_wav_len; }
  else        { g_clipData = replenish_wav;      g_clipLen = replenish_wav_len; }
#else
  g_clipPath = bonus ? CLIP_PATH_BONUS : CLIP_PATH;
#endif
}

// ---- public API -------------------------------------------------------------

bool startAudio() {
  if (playing) return true;
  playing = openChain();
  return playing;
}

void stopAudio() {
  if (!playing) return;
  if (decoder) decoder->stop();     // clean stop so next begin() works
  playing = false;
}

// Feed decoder; on EOF either stop (one-shot/chime) or re-open for loop
void handleAudio() {
  if (!playing || !decoder) return;

  if (!decoder->loop()) {                 // EOF or starvation
    decoder->stop();

    if (g_audioOneShot || g_chimeActive) {
      // one-shot or chime finished
      playing = false;

      // If we just finished the spawn chime, don't resume replenish here
      if (g_chimeActive) {
        g_chimeActive = false;
        g_audioOneShot = false;
        g_resumeLoopAfterChime = false;
      } else {
        // regular one-shot done
        g_audioOneShot = false;
        if (g_resumeLoopAfterChime && holdActive) {
          g_resumeLoopAfterChime = false;
          selectClip(false);             // normal replenish loop
          startAudio();
        }
      }
    } else {
      // normal replenish loop: keep looping
      playing = openChain();
      if (!playing) Serial.println("[LOOT] audio re-begin failed");
    }
  }
}

// Select correct clip (bonus=one-shot w/ short exclusive window) and start
bool startLootAudio(bool bonus) {
  g_audioOneShot = bonus;                 // bonus => one-shot behavior
  if (bonus) {
    g_bonusExclusiveUntilMs = millis() + 350;
    if (playing) stopAudio();             // ensure bonus clip actually starts now
  } else {
    g_bonusExclusiveUntilMs = 0;
  }
  selectClip(bonus);
  return startAudio();
}

// Pre-empts replenish loop and plays spawn chime fully, not stopped by HOLD_END
void playBonusSpawnChime() {
  if (g_chimeActive) return;              // already playing a chime

  const bool wasLooping = playing && !g_audioOneShot;
  if (wasLooping) {
    stopAudio();                          // cleanly stop replenish loop
    g_resumeLoopAfterChime = false;       // do not resume after chime
  }

  g_chimeActive  = true;
  g_audioOneShot = true;                  // ensures we don't auto-loop

#if TREX_AUDIO_PROGMEM
  const uint8_t* savedData = g_clipData; size_t savedLen = g_clipLen;
  g_clipData = bonus_spawn_wav; g_clipLen = bonus_spawn_wav_len;
  startAudio();
  g_clipData = savedData; g_clipLen = savedLen;
#else
  const char* savedPath = g_clipPath;
  g_clipPath = CLIP_PATH_SPAWN;
  startAudio();
  g_clipPath = savedPath;
#endif
}

static uint32_t schedAudioStopAt = 0;

void scheduleAudioStop(uint16_t delayMs) {
  schedAudioStopAt = millis() + delayMs;
}

void tickScheduledAudio() {
  if (schedAudioStopAt && (int32_t)(millis() - schedAudioStopAt) >= 0) {
    // Don't kill bonus one-shots or the spawn chime
    if (!g_audioOneShot && !g_chimeActive) {
      stopAudio();
    }
    schedAudioStopAt = 0;
  }
}
