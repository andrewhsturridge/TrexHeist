#include "MaintCommands.h"
#include "TrexMaintenance.h"   // uses the custom handler hook
#include "Media.h"
#include "Net.h"
#include "Cadence.h"
#include <WiFi.h>

static Game* GP = nullptr;

static bool parseUint(const String& s, uint32_t& out) {
  char* end=nullptr;
  out = strtoul(s.c_str(), &end, 10);
  return end && *end=='\0';
}
static bool parseInt(const String& s, int32_t& out) {
  char* end=nullptr;
  out = strtol(s.c_str(), &end, 10);
  return end && *end=='\0';
}

static void printStatus(WiFiClient& out, Game& g) {
  out.printf("phase=%s light=%s score=%u \n",
             (g.phase==Phase::PLAYING?"PLAYING":"END"),
             (g.light==LightState::GREEN?"GREEN":"RED"),
             (unsigned)g.teamScore);
  out.printf("G=%u R=%u loot=%u maxCarry=%u tickHz=%u pir=%u pirArm=%u\n",
             (unsigned)g.greenMs, (unsigned)g.redMs, (unsigned)g.lootRateMs,
             (unsigned)g.maxCarry, (unsigned)g.tickHz,
             (unsigned)g.pirEnforce, (unsigned)g.pirArmDelayMs);
  out.printf("edgeGrace=%u redHoldGrace=%u\n",
             (unsigned)g.edgeGraceMs, (unsigned)g.redHoldGraceMs);

  // holds summary
  int active=0; for (auto &h : g.holds) if (h.active) active++;
  out.printf("holdsActive=%d\n", active);

  // station table
  for (uint8_t sid=1; sid<=5; ++sid) {
    out.printf("station %u: inv=%u/%u\n", sid, (unsigned)g.stationInventory[sid], (unsigned)g.stationCapacity[sid]);
  }
}

static bool handleCmd(const String& raw, WiFiClient& out) {
  if (!GP) return false;
  Game& g = *GP;

  String cmd = raw; // already lowercased by TrexMaintenance
  // split into tokens
  auto nextTok = [&](int& i)->String{
    while (i < (int)cmd.length() && cmd[i]==' ') i++;
    int start=i;
    while (i < (int)cmd.length() && cmd[i]!=' ') i++;
    return cmd.substring(start,i);
  };

  int i=0;
  String t = nextTok(i);

  if (t=="status") { printStatus(out, g); return true; }

  if (t=="set") {
    String key = nextTok(i); String val = nextTok(i);
    uint32_t u=0; if (!parseUint(val,u)) { out.print("bad value\n"); return true; }

    if (key=="green_ms")      g.greenMs = u;
    else if (key=="red_ms")   g.redMs = u;
    else if (key=="loot_ms")  g.lootRateMs = u;
    else if (key=="max_carry"){ g.maxCarry = (uint8_t)u; }
    else if (key=="edge_grace_ms") g.edgeGraceMs = u;
    else if (key=="red_hold_grace_ms") g.redHoldGraceMs = u;
    else if (key=="pir_arm_ms") g.pirArmDelayMs = u;
    else if (key=="tick_hz")  { g.tickHz = (uint8_t)max<uint32_t>(1,u); }
    else { out.print("unknown key\n"); return true; }

    out.print("ok\n");
    return true;
  }

  if (t=="pir") {
    String v = nextTok(i);
    if (v=="on")  g.pirEnforce = true;
    else if (v=="off") g.pirEnforce = false;
    else { out.print("usage: pir on|off\n"); return true; }
    out.print("ok\n");
    return true;
  }

  if (t=="cap" || t=="inv" || t=="fill" || t=="drain") {
    if (t=="fill") {
      String sid = nextTok(i);
      if (sid=="all") {
        for (uint8_t s=1;s<=5;s++) g.stationInventory[s]=g.stationCapacity[s];
        out.print("ok\n"); return true;
      }
      uint32_t s=0; if(!parseUint(sid,s) || s<1 || s>5){out.print("bad sid\n"); return true;}
      g.stationInventory[s]=g.stationCapacity[s];
      out.print("ok\n"); return true;
    }
    if (t=="drain") {
      String sidS = nextTok(i), nS = nextTok(i);
      uint32_t s=0,n=0; if(!parseUint(sidS,s) || s<1 || s>5 || !parseUint(nS,n)){out.print("usage: drain <sid> <n>\n"); return true;}
      uint16_t cur = g.stationInventory[s];
      g.stationInventory[s] = (cur > n) ? (cur - n) : 0;
      out.print("ok\n"); return true;
    }
    if (t=="cap") {
      String sidS = nextTok(i), capS = nextTok(i);
      uint32_t s=0,c=0; if(!parseUint(sidS,s) || s<1 || s>5 || !parseUint(capS,c)){out.print("usage: cap <sid> <cap>\n"); return true;}
      g.stationCapacity[s]=(uint16_t)c;
      if (g.stationInventory[s] > g.stationCapacity[s]) g.stationInventory[s]=g.stationCapacity[s];
      out.print("ok\n"); return true;
    }
    if (t=="inv") {
      String sidS = nextTok(i), invS = nextTok(i);
      uint32_t s=0,v=0; if(!parseUint(sidS,s) || s<1 || s>5 || !parseUint(invS,v)){out.print("usage: inv <sid> <inv>\n"); return true;}
      g.stationInventory[s]=(uint16_t)min<uint32_t>(v, g.stationCapacity[s]);
      out.print("ok\n"); return true;
    }
  }

  if (t=="score") {
    String deltaS = nextTok(i);
    int32_t d=0; if(!parseInt(deltaS,d)){out.print("usage: score +/-N\n"); return true;}
    int32_t ns = (int32_t)g.teamScore + d; if (ns<0) ns=0; g.teamScore=(uint32_t)ns;
    bcastScore(g);
    out.print("ok\n"); return true;
  }

  if (t=="sprite") {
    String clipS = nextTok(i); uint32_t c=0; if(!parseUint(clipS,c)){out.print("usage: sprite <clip>\n"); return true;}
    spritePlay((uint8_t)c); out.print("ok\n"); return true;
  }

  if (t=="new") { startNewGame(g); out.print("ok\n"); return true; }
  if (t=="end") { bcastGameOver(g, /*MANUAL*/2); out.print("ok\n"); return true; }
  if (t=="green"){ enterGreen(g); out.print("ok\n"); return true; }
  if (t=="red")  { enterRed(g);  out.print("ok\n"); return true; }

  return false; // not handled
}

void maintRegisterServerCommands(Game& g) {
  GP = &g;
  Maint::CustomHandler() = handleCmd;   // hook into TrexMaintenance telnet
}
