#include "GameAudio.h"
#include <HardwareSerial.h>
#include <DYPlayerArduino.h>

static HardwareSerial AudioSerial(2);      // UART2 on ESP32-S3
static DY::Player     audioModule(&AudioSerial);
static uint16_t       g_currentTrack = 0;

void gameAudioInit(uint8_t rxPin, uint8_t txPin, uint32_t baud, uint8_t volume) {
  AudioSerial.begin(baud, SERIAL_8N1, rxPin, txPin);
  audioModule.begin();
  audioModule.setCycleMode(DY::PlayMode::OneOff);
  audioModule.setVolume(volume);           // 0..30 typical on DY modules
  g_currentTrack = 0;
}

void gameAudioPlayOnce(uint16_t track) {
  audioModule.playSpecified(track);
  g_currentTrack = track;
}

void gameAudioStop() {
  audioModule.stop();
  g_currentTrack = 0;
}

uint16_t gameAudioCurrentTrack() {   // ← accessor
  return g_currentTrack;
}
