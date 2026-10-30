#pragma once
// All settings live in one plain struct, loaded from /settings.json at boot.
// Draw code reads fields directly (no JSON lookups per frame, which was a big
// CPU cost before). Only the UI loop writes it, under cfgLock(); the web and
// net tasks read under cfgLock().
#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"

struct WifiNet { char ssid[33]; char pass[65]; };
struct Alarm { bool on; uint8_t h, m; uint8_t days; char label[20]; };   // days bit0 = Mon .. bit6 = Sun, 0 = once
struct TeamRef { char id[12]; char lg[20]; char abbr[6]; };

enum SoundEvent : uint8_t { SE_UI = 0, SE_ALARM, SE_TIMER, SE_SPORTS, SE_MSG, SE_EMO, SE_HOURLY, SE_COUNT };

struct Settings {
  // device
  char host[24];
  char pin[12];
  // wi-fi
  WifiNet wifi[MAX_WIFI];
  uint8_t txPower;          // 0 = 8.5 dBm (best for Super Mini), 1 = 11, 2 = 15, 3 = 19.5
  bool wifiSleep;
  // time & place
  char tz[48];              // POSIX TZ, e.g. "IST-5:30"
  char tzName[40];          // IANA, e.g. "Asia/Kolkata" (for the hub's calendar)
  bool h24;
  float lat, lon;
  char place[32];
  bool imperial;
  // display
  uint8_t bright;           // 0..255
  bool nightDim;
  char dimFrom[6], dimTo[6];
  uint8_t dimLevel;
  bool flip;
  bool burnShift;
  bool slide;               // slide between cards
  uint16_t offAfterMin;     // screen off after no touch (0 = never)
  uint32_t i2cHz;
  // rotation
  bool rotate;
  uint8_t rotateSec;
  uint16_t homeAfterSec;    // with rotation off: go back to Emo after this long (0 = never)
  bool skipEmpty;
  // emo
  uint8_t lively;           // 0..100
  char sleepAt[6], wakeAt[6];
  bool reactWeather, reactSports;
  // hub
  char hubUrl[96];
  char hubToken[40];
  // sports
  TeamRef fbTeams[6];   uint8_t nFbTeams;
  char fbLeagues[4][20]; uint8_t nFbLeagues;
  TeamRef crTeams[6];   uint8_t nCrTeams;
  bool crAll;
  bool alGoal, alWkt, alRes, alStart, alAny;
  // markets / news / calendar (comma separated lists)
  char crypto[96], stocks[96], fx[64], vs[6];
  char news[120];
  char ical[200];
  // sound
  bool sound;               // speaker installed
  uint8_t volume;           // 0..100
  char quietFrom[6], quietTo[6];
  uint8_t sndMask;          // bit per SoundEvent
  bool hourly;
  // alarms
  Alarm alarms[MAX_ALARMS];
  uint8_t nAlarms;
  // input
  uint16_t multiMs, holdMs, longMs;
  // pc companion
  bool pcPresence;
};

enum CardTypeId : uint8_t {
  CT_EMO = 0, CT_CLOCK, CT_ANALOG, CT_WEATHER, CT_FORECAST, CT_AIR, CT_CRICKET, CT_FOOTBALL, CT_TABLE,
  CT_MARKETS, CT_NEWS, CT_QUOTE, CT_AGENDA, CT_POMODORO, CT_TIMER, CT_STOPWATCH, CT_ALARMS, CT_PC,
  CT_MESSAGES, CT_SYSTEM, CT_DINO, CT_COUNT
};

struct CardCfg {
  uint8_t type;
  bool on;
  uint8_t dur;              // seconds on screen during rotation, 0 = default
  char arg[24];             // per-type option, e.g. league "eng.1" or markets filter "c"
  char name[16];            // custom title, "" = default
};

struct CardTypeInfo {
  const char* key;          // JSON id
  const char* title;        // default title on screen
  const char* desc;         // dashboard description
  const char* argHelp;      // "" = no option
  bool needsHub;
};
extern const CardTypeInfo CARD_TYPES[CT_COUNT];
int cardTypeFromKey(const char* k);

extern Settings S;
extern CardCfg gCards[MAX_CARDS];
extern uint8_t gCardN;
extern volatile uint32_t gCfgRev;        // bumps on every applied change

void cfgLock();
void cfgUnlock();

void settingsBegin();                      // load or create defaults
void settingsDefaults(Settings& s);
void cardsDefaults();
void settingsToJson(JsonObject o, bool secrets);
bool settingsFromJson(JsonObjectConst o);  // partial merge, returns true if anything changed
void cardsToJson(JsonArray a);
bool cardsFromJson(JsonArrayConst a);
void settingsMarkDirty();
void settingsLoop();                        // debounced save
void settingsSaveNow();
void settingsFactoryReset();

// The subscription sent to the hub, built from settings + enabled cards.
void buildSubscription(JsonDocument& d);
bool cardEnabled(uint8_t type);
