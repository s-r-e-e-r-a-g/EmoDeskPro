#pragma once
// Live data, as delivered by the hub (or the direct weather fallback).
// Fixed-size POD structs: no heap churn, no fragmentation. The net task fills
// a back buffer and swaps it in under a mutex held for microseconds.
#include <Arduino.h>

struct WxHour { uint8_t h; uint8_t code; int8_t pop; int16_t t10; };
struct WxDay  { uint8_t wd; uint8_t code; int8_t pop; int16_t hi10, lo10; };
struct Weather {
  bool ok;
  int16_t t10, f10, wind10;           // temperature, feels like, wind (x10)
  uint8_t hum, code, uv10;
  bool day;
  int16_t hi10, lo10;
  char sr[6], ss[6];
  WxHour hr[12]; uint8_t nh;
  WxDay dy[6];   uint8_t nd;
};
struct Air { bool ok; int16_t aqi; int16_t pm25x10, pm10x10, uv10; };

enum MatchState : uint8_t { MS_PRE = 0, MS_IN = 1, MS_POST = 2 };

struct FbSide { char id[10]; char a[6]; char n[20]; int8_t s; uint8_t red; char goals[50]; };
struct FbMatch {
  char id[12];
  char lg[6];
  char lid[20];                       // ESPN league slug, e.g. "eng.1"
  uint8_t st;
  char d[16];                         // "67'", "HT", "FT"
  uint32_t t;                         // kickoff (unix)
  FbSide h, a;
  bool mine;
};
struct CrSide { char a[6]; char n[22]; char sc[20]; char ov[6]; bool bat, win; };
struct CrMatch {
  char id[12];
  char ser[28];
  char ti[28];
  char f[5];
  uint8_t st;
  char status[62];
  uint32_t t;
  CrSide s[2];
  int16_t crr10, rrr10;
  char need[16];
  bool mine;
};
struct TableRow { uint8_t pos; char a[6]; int16_t pts; uint8_t gp; int16_t gd; bool mine; };
struct Table { char lg[24]; char lid[20]; TableRow r[24]; uint8_t n; };
struct Ticker { char s[10]; char p[16]; int16_t c10; uint8_t sp[24]; uint8_t nsp; char k; };
struct Headline { char h[112]; char s[16]; };
struct Quote { char q[142]; char a[26]; };
struct CalEv { uint32_t t, e; char d[42]; bool ad; };

struct Feed {
  uint32_t v;                         // hub feed version
  uint32_t gotAt;                     // millis() when received
  Weather wx;
  Air aq;
  FbMatch fb[14]; uint8_t nfb;
  CrMatch cr[6];  uint8_t ncr;
  Table tb[3];    uint8_t ntb;
  Ticker mk[16];  uint8_t nmk;
  Headline nw[8]; uint8_t nnw;
  Quote qt;       bool hasQt;
  CalEv ev[8];    uint8_t nev;
};

// PC companion (posted straight to the device on the LAN)
struct PcStats {
  uint32_t at;                        // millis() of last report, 0 = never
  float cpu, ram, gpu, cpuT, gpuT;
  char song[48], artist[32];
  bool playing;
  uint32_t idleS;
  bool locked;
};

// Messages from Telegram / hub push / HTTP API
struct Message { char from[20]; char text[200]; uint32_t at; };

enum HubState : uint8_t { HUB_OFF = 0, HUB_CONNECTING, HUB_OK, HUB_ERROR };

// Access (UI loop): feedLock(); const Feed& f = feed(); ... feedUnlock();
void feedLock();
void feedUnlock();
const Feed& feed();
uint32_t feedVersion();                // bumps on every swap (UI redraw trigger)

extern PcStats gPc;
extern Message gMsgs[4];
extern uint8_t gMsgN;
void addMessage(const char* from, const char* text);

// Hub client / network task
void netTaskBegin();
HubState hubState();
const char* hubError();
uint32_t hubLastOkS();                 // uptime seconds of last good feed, 0 = never
bool feedHasLiveMine();
