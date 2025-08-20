#include "Media.h"
#include <Arduino.h>

void mediaInit() {
  Serial1.begin(SPRITE_BAUD, SERIAL_8N1, SPRITE_RX, SPRITE_TX);
  delay(20);
}

void spritePlay(uint8_t clip) {
  Serial.printf("[TREX] Sprite -> play clip %u\n", clip);
  Serial1.write(clip);  // Sprite expects single-byte clip numbers
}
