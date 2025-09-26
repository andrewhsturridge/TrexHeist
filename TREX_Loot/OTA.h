#pragma once
#include <stdint.h>
#include "TrexProtocol.h"   // for OtaPhase, MsgHeader, OtaStatusPayload, StationType

// Exact functions moved out of TREX_Loot.ino (no changes):
void sendOtaStatus(OtaPhase phase, uint8_t errCode, uint32_t bytes, uint32_t total);
void otaWriteFile(bool successPending);
bool otaReadFile(uint32_t &campId, bool &successPending);
void otaClearFile();
bool doOtaFromUrlDetailed(const char* url);
