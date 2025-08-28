#include "GameAudio.h"
#include <HardwareSerial.h>
#include <DYPlayerArduino.h>

static HardwareSerial AudioSerial(2);      // UART2 on ESP32-S3
static DY::Player     audioModule(&AudioSerial);

// Simple loop supervisor (library-agnostic)
static uint16_t g_currentTrack = 0;
static bool     g_isLooping    = false;
static uint32_t g_lastKickMs   = 0;
static uint32_t g_loopTrack    = 0;

// Re-trigger period for looped tracks (tune to clip length to avoid gaps)
static uint32_t g_loopRetriggerMs = 1200;  // ~1.2s for "TicksLoop"

void gameAudioInit(uint8_t rxPin, uint8_t txPin, uint32_t baud, uint8_t volume) {
  AudioSerial.begin(baud, SERIAL_8N1, rxPin, txPin);
  audioModule.begin();
  audioModule.setVolume(volume);           // 0..30 typical on DY modules
  g_currentTrack = 0; g_isLooping = false; g_loopTrack = 0; g_lastKickMs = 0;
}

void gameAudioPlayOnce(uint16_t track) {
  audioModule.playSpecified(track);
  g_currentTrack = track;
  g_isLooping = false;
  g_loopTrack = 0;
  g_lastKickMs = millis();
}

void gameAudioPlayLoop(uint16_t track) {
  // Many DY libs don’t expose a “loop one” API; emulate by periodic re-triggers.
  // Works fine for ambience/beeps. If your lib adds a loop call later, drop it in here.
  audioModule.playSpecified(track);
  g_currentTrack = track;
  g_isLooping = true;
  g_loopTrack = track;
  g_lastKickMs = millis();
}

void gameAudioStop() {
  audioModule.stop();
  g_currentTrack = 0;
  g_isLooping = false;
  g_loopTrack = 0;
}

void gameAudioStopIfLooping() {
  if (g_isLooping) gameAudioStop();
}

void gameAudioLoopTick() {
  if (!g_isLooping || g_loopTrack == 0) return;
  const uint32_t now = millis();
  if (now - g_lastKickMs >= g_loopRetriggerMs) {
    audioModule.playSpecified(g_loopTrack);  // re-issue to keep it looping
    g_lastKickMs = now;
  }
}
