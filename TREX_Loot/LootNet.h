#pragma once
#include <stdint.h>
#include <TrexProtocol.h>   // MsgHeader, MsgType, HelloPayload, Loot*Payload, TrexUid

// Moved as-is from TREX_Loot.ino:
void packHeader(uint8_t type, uint16_t payLen, uint8_t* buf);
void sendHello();
void sendHoldStart(const TrexUid& uid);
void sendHoldStop();
void sendMgResult(const TrexUid& uid, uint8_t success);
