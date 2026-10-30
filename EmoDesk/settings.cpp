#include "settings.h"
#include "sys.h"
#include <LittleFS.h>
#include <freertos/semphr.h>

Settings S;
CardCfg gCards[MAX_CARDS];
uint8_t gCardN = 0;
volatile uint32_t gCfgRev = 1;

static SemaphoreHandle_t cfgMx = nullptr;
static uint32_t dirtyAt = 0;
static const char* MASK = "********";

const CardTypeInfo CARD_TYPES[CT_COUNT] = {
  {"emo", "Emo", "Emo's face. Hold to pet, double-tap to poke.", "", false},
  {"clock", "Clock", "Big digital clock with the date.", "", false},
  {"analog", "Analog", "Analog clock face.", "", false},
  {"weather", "Weather", "Current weather and the next hours.", "", false},
  {"forecast", "Forecast", "Five-day forecast.", "", true},
  {"air", "Air", "Air quality index and particles.", "", true},
  {"cricket", "Cricket", "Live scores for the teams you follow.", "", true},
  {"football", "Football", "Live scores, next and last match.", "League code to show only one league, e.g. eng.1", true},
  {"table", "Table", "League table.", "League code, e.g. eng.1", true},
  {"markets", "Markets", "Crypto, stocks and currency.", "c = crypto only, s = stocks, f = currency", true},
  {"news", "News", "Headlines.", "", true},
  {"quote", "Quote", "Quote of the hour.", "", true},
  {"agenda", "Agenda", "Upcoming calendar events.", "", true},
  {"pomodoro", "Focus", "25/5 focus timer. Double-tap start/pause, hold reset.", "Minutes, e.g. 25/5", false},
  {"timer", "Timer", "Countdown. Hold picks a preset, double-tap start/pause.", "Minutes list, e.g. 1,3,5,10,15,30", false},
  {"stopwatch", "Stopwatch", "Double-tap start/stop, hold lap/reset.", "", false},
  {"alarms", "Alarms", "Next alarm. Double-tap turns it on/off.", "", false},
  {"pc", "PC", "CPU, RAM and now playing from the PC companion.", "", false},
  {"messages", "Messages", "Last messages from Telegram, the hub or the API.", "", false},
  {"system", "System", "Wi-Fi, address, hub status. QR code opens the dashboard.", "", false},
  {"dino", "Dino", "One-button game. Touch to jump, hold to leave.", "", false},
};

int cardTypeFromKey(const char* k) {
  if (!k) return -1;
  for (int i = 0; i < CT_COUNT; i++) if (!strcmp(CARD_TYPES[i].key, k)) return i;
  return -1;
}

void cfgLock() { if (cfgMx) xSemaphoreTakeRecursive(cfgMx, portMAX_DELAY); }
void cfgUnlock() { if (cfgMx) xSemaphoreGiveRecursive(cfgMx); }

void settingsDefaults(Settings& s) {
  memset(&s, 0, sizeof(s));
  safeCopy(s.host, DEFAULT_HOST, sizeof(s.host));
  s.txPower = 0;
  s.wifiSleep = false;
  safeCopy(s.tz, "UTC0", sizeof(s.tz));
  safeCopy(s.tzName, "UTC", sizeof(s.tzName));
  s.h24 = false;
  s.bright = 160;
  s.nightDim = true;
  safeCopy(s.dimFrom, "22:30", 6);
  safeCopy(s.dimTo, "07:00", 6);
  s.dimLevel = 8;
  s.burnShift = true;
  s.slide = true;
  s.i2cHz = OLED_I2C_HZ_DEFAULT;
  s.rotate = true;
  s.rotateSec = 10;
  s.homeAfterSec = 60;
  s.skipEmpty = true;
  s.lively = 60;
  safeCopy(s.sleepAt, "23:30", 6);
  safeCopy(s.wakeAt, "07:00", 6);
  s.reactWeather = true;
  s.reactSports = true;
  s.alGoal = s.alWkt = s.alRes = s.alStart = true;
  safeCopy(s.crypto, "bitcoin,ethereum", sizeof(s.crypto));
  safeCopy(s.vs, "usd", sizeof(s.vs));
  safeCopy(s.news, "bbc-world", sizeof(s.news));
  s.volume = 40;
  safeCopy(s.quietFrom, "22:00", 6);
  safeCopy(s.quietTo, "08:00", 6);
  s.sndMask = 0xFF;
  s.multiMs = 260;
  s.holdMs = 600;
  s.longMs = 2500;
  s.pcPresence = true;
}

void cardsDefaults() {
  const uint8_t def[] = {CT_EMO, CT_CLOCK, CT_WEATHER, CT_FOOTBALL, CT_CRICKET, CT_MARKETS, CT_NEWS, CT_AGENDA, CT_POMODORO, CT_SYSTEM};
  gCardN = 0;
  for (uint8_t t : def) {
    CardCfg& c = gCards[gCardN++];
    memset(&c, 0, sizeof(c));
    c.type = t;
    c.on = true;
    c.dur = t == CT_EMO ? 20 : 0;
  }
}

// ------------------------------------------------------------------ JSON helpers
template <typename T> static bool setNum(JsonObjectConst o, const char* k, T& v, T lo, T hi) {
  JsonVariantConst x = o[k];
  if (x.isNull() || !(x.is<float>() || x.is<int>() || x.is<long>())) return false;
  T n = (T)x.as<double>();
  if (n < lo) n = lo;
  if (n > hi) n = hi;
  if (n == v) return false;
  v = n;
  return true;
}
static bool setBool(JsonObjectConst o, const char* k, bool& v) {
  JsonVariantConst x = o[k];
  if (!x.is<bool>()) return false;
  bool n = x.as<bool>();
  if (n == v) return false;
  v = n;
  return true;
}
static bool setStr(JsonObjectConst o, const char* k, char* v, size_t n) {
  JsonVariantConst x = o[k];
  if (!x.is<const char*>()) return false;
  const char* s = x.as<const char*>();
  if (!strcmp(s, MASK)) return false;           // masked secret: unchanged
  if (!strncmp(v, s, n)) return false;
  safeCopy(v, s, n);
  return true;
}
static bool setHM(JsonObjectConst o, const char* k, char* v) {
  JsonVariantConst x = o[k];
  if (!x.is<const char*>() || parseHM(x.as<const char*>()) < 0) return false;
  return setStr(o, k, v, 6);
}
static void teamsTo(JsonArray a, const TeamRef* t, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    JsonObject o = a.add<JsonObject>();
    o["id"] = t[i].id;
    o["lg"] = t[i].lg;
    o["abbr"] = t[i].abbr;
  }
}
static bool teamsFrom(JsonVariantConst v, TeamRef* t, uint8_t& n, uint8_t max) {
  if (!v.is<JsonArrayConst>()) return false;
  TeamRef tmp[6];
  memset(tmp, 0, sizeof(tmp));
  uint8_t k = 0;
  for (JsonObjectConst o : v.as<JsonArrayConst>()) {
    if (k >= max) break;
    safeCopy(tmp[k].id, o["id"] | "", sizeof(tmp[k].id));
    safeCopy(tmp[k].lg, o["lg"] | "", sizeof(tmp[k].lg));
    safeCopy(tmp[k].abbr, o["abbr"] | "", sizeof(tmp[k].abbr));
    if (tmp[k].id[0] || tmp[k].abbr[0]) k++;
  }
  if (k == n && !memcmp(tmp, t, sizeof(TeamRef) * k)) return false;
  memcpy(t, tmp, sizeof(TeamRef) * max);
  n = k;
  return true;
}

// ------------------------------------------------------------------ settings <-> JSON
void settingsToJson(JsonObject o, bool secrets) {
  JsonObject d = o["device"].to<JsonObject>();
  d["host"] = S.host;
  d["pin"] = secrets ? S.pin : (S.pin[0] ? MASK : "");

  JsonObject w = o["wifi"].to<JsonObject>();
  JsonArray nets = w["nets"].to<JsonArray>();
  for (int i = 0; i < MAX_WIFI; i++) {
    if (!S.wifi[i].ssid[0]) continue;
    JsonObject n = nets.add<JsonObject>();
    n["ssid"] = S.wifi[i].ssid;
    n["pass"] = secrets ? S.wifi[i].pass : (S.wifi[i].pass[0] ? MASK : "");
  }
  w["tx"] = S.txPower;
  w["sleep"] = S.wifiSleep;

  JsonObject t = o["time"].to<JsonObject>();
  t["tz"] = S.tz;
  t["tzName"] = S.tzName;
  t["h24"] = S.h24;

  JsonObject l = o["loc"].to<JsonObject>();
  l["lat"] = S.lat;
  l["lon"] = S.lon;
  l["place"] = S.place;
  l["imperial"] = S.imperial;

  JsonObject di = o["display"].to<JsonObject>();
  di["bright"] = S.bright;
  di["nightDim"] = S.nightDim;
  di["dimFrom"] = S.dimFrom;
  di["dimTo"] = S.dimTo;
  di["dimLevel"] = S.dimLevel;
  di["flip"] = S.flip;
  di["burnShift"] = S.burnShift;
  di["slide"] = S.slide;
  di["offAfterMin"] = S.offAfterMin;
  di["i2cHz"] = S.i2cHz;

  JsonObject r = o["rotate"].to<JsonObject>();
  r["on"] = S.rotate;
  r["sec"] = S.rotateSec;
  r["homeAfter"] = S.homeAfterSec;
  r["skipEmpty"] = S.skipEmpty;

  JsonObject e = o["emo"].to<JsonObject>();
  e["lively"] = S.lively;
  e["sleepAt"] = S.sleepAt;
  e["wakeAt"] = S.wakeAt;
  e["weather"] = S.reactWeather;
  e["sports"] = S.reactSports;

  JsonObject h = o["hub"].to<JsonObject>();
  h["url"] = S.hubUrl;
  h["token"] = secrets ? S.hubToken : (S.hubToken[0] ? MASK : "");

  JsonObject sp = o["sports"].to<JsonObject>();
  teamsTo(sp["fbTeams"].to<JsonArray>(), S.fbTeams, S.nFbTeams);
  JsonArray lg = sp["fbLeagues"].to<JsonArray>();
  for (int i = 0; i < S.nFbLeagues; i++) lg.add(S.fbLeagues[i]);
  teamsTo(sp["crTeams"].to<JsonArray>(), S.crTeams, S.nCrTeams);
  sp["crAll"] = S.crAll;
  sp["alGoal"] = S.alGoal;
  sp["alWkt"] = S.alWkt;
  sp["alRes"] = S.alRes;
  sp["alStart"] = S.alStart;
  sp["alAny"] = S.alAny;

  JsonObject m = o["markets"].to<JsonObject>();
  m["crypto"] = S.crypto;
  m["stocks"] = S.stocks;
  m["fx"] = S.fx;
  m["vs"] = S.vs;
  o["news"] = S.news;
  o["ical"] = S.ical;

  JsonObject so = o["sound"].to<JsonObject>();
  so["on"] = S.sound;
  so["volume"] = S.volume;
  so["quietFrom"] = S.quietFrom;
  so["quietTo"] = S.quietTo;
  so["mask"] = S.sndMask;
  so["hourly"] = S.hourly;

  JsonArray al = o["alarms"].to<JsonArray>();
  for (int i = 0; i < S.nAlarms; i++) {
    JsonObject a = al.add<JsonObject>();
    a["on"] = S.alarms[i].on;
    char hm[8];
    snprintf(hm, sizeof(hm), "%02d:%02d", S.alarms[i].h, S.alarms[i].m);
    a["time"] = hm;
    a["days"] = S.alarms[i].days;
    a["label"] = S.alarms[i].label;
  }

  JsonObject in = o["input"].to<JsonObject>();
  in["multiMs"] = S.multiMs;
  in["holdMs"] = S.holdMs;
  in["longMs"] = S.longMs;
  o["pcPresence"] = S.pcPresence;
}

bool settingsFromJson(JsonObjectConst o) {
  bool ch = false;
  JsonObjectConst d = o["device"];
  if (!d.isNull()) {
    JsonVariantConst hv = d["host"];
    if (hv.is<const char*>()) {
      // hostnames: letters, digits, dash
      char h[24];
      size_t k = 0;
      for (const char* p = hv.as<const char*>(); *p && k < sizeof(h) - 1; p++) {
        char c = tolower(*p);
        if (isalnum((unsigned char)c) || c == '-') h[k++] = c;
      }
      h[k] = 0;
      if (k && strcmp(h, S.host)) { safeCopy(S.host, h, sizeof(S.host)); ch = true; }
    }
    ch |= setStr(d, "pin", S.pin, sizeof(S.pin));
  }
  JsonObjectConst w = o["wifi"];
  if (!w.isNull()) {
    if (w["nets"].is<JsonArrayConst>()) {
      WifiNet nets[MAX_WIFI];
      memset(nets, 0, sizeof(nets));
      int k = 0;
      for (JsonObjectConst n : w["nets"].as<JsonArrayConst>()) {
        if (k >= MAX_WIFI) break;
        const char* ssid = n["ssid"] | "";
        if (!*ssid) continue;
        safeCopy(nets[k].ssid, ssid, sizeof(nets[k].ssid));
        const char* pass = n["pass"] | MASK;
        if (!strcmp(pass, MASK)) {      // keep the stored password for this ssid
          for (int i = 0; i < MAX_WIFI; i++) if (!strcmp(S.wifi[i].ssid, ssid)) safeCopy(nets[k].pass, S.wifi[i].pass, sizeof(nets[k].pass));
        } else safeCopy(nets[k].pass, pass, sizeof(nets[k].pass));
        k++;
      }
      if (memcmp(nets, S.wifi, sizeof(nets))) { memcpy(S.wifi, nets, sizeof(nets)); ch = true; }
    }
    ch |= setNum<uint8_t>(w, "tx", S.txPower, 0, 3);
    ch |= setBool(w, "sleep", S.wifiSleep);
  }
  JsonObjectConst t = o["time"];
  if (!t.isNull()) {
    ch |= setStr(t, "tz", S.tz, sizeof(S.tz));
    ch |= setStr(t, "tzName", S.tzName, sizeof(S.tzName));
    ch |= setBool(t, "h24", S.h24);
  }
  JsonObjectConst l = o["loc"];
  if (!l.isNull()) {
    ch |= setNum<float>(l, "lat", S.lat, -90, 90);
    ch |= setNum<float>(l, "lon", S.lon, -180, 180);
    ch |= setStr(l, "place", S.place, sizeof(S.place));
    ch |= setBool(l, "imperial", S.imperial);
  }
  JsonObjectConst di = o["display"];
  if (!di.isNull()) {
    ch |= setNum<uint8_t>(di, "bright", S.bright, 1, 255);
    ch |= setBool(di, "nightDim", S.nightDim);
    ch |= setHM(di, "dimFrom", S.dimFrom);
    ch |= setHM(di, "dimTo", S.dimTo);
    ch |= setNum<uint8_t>(di, "dimLevel", S.dimLevel, 1, 255);
    ch |= setBool(di, "flip", S.flip);
    ch |= setBool(di, "burnShift", S.burnShift);
    ch |= setBool(di, "slide", S.slide);
    ch |= setNum<uint16_t>(di, "offAfterMin", S.offAfterMin, 0, 600);
    ch |= setNum<uint32_t>(di, "i2cHz", S.i2cHz, 100000, 1000000);
  }
  JsonObjectConst r = o["rotate"];
  if (!r.isNull()) {
    ch |= setBool(r, "on", S.rotate);
    ch |= setNum<uint8_t>(r, "sec", S.rotateSec, 3, 240);
    ch |= setNum<uint16_t>(r, "homeAfter", S.homeAfterSec, 0, 3600);
    ch |= setBool(r, "skipEmpty", S.skipEmpty);
  }
  JsonObjectConst e = o["emo"];
  if (!e.isNull()) {
    ch |= setNum<uint8_t>(e, "lively", S.lively, 0, 100);
    ch |= setHM(e, "sleepAt", S.sleepAt);
    ch |= setHM(e, "wakeAt", S.wakeAt);
    ch |= setBool(e, "weather", S.reactWeather);
    ch |= setBool(e, "sports", S.reactSports);
  }
  JsonObjectConst h = o["hub"];
  if (!h.isNull()) {
    if (h["url"].is<const char*>()) {
      String u = h["url"].as<const char*>();
      u.trim();
      while (u.endsWith("/")) u.remove(u.length() - 1);
      if (u.length() && !u.startsWith("http://") && !u.startsWith("https://")) u = "http://" + u;
      if (strcmp(u.c_str(), S.hubUrl)) { safeCopy(S.hubUrl, u.c_str(), sizeof(S.hubUrl)); ch = true; }
    }
    ch |= setStr(h, "token", S.hubToken, sizeof(S.hubToken));
  }
  JsonObjectConst sp = o["sports"];
  if (!sp.isNull()) {
    ch |= teamsFrom(sp["fbTeams"], S.fbTeams, S.nFbTeams, 6);
    ch |= teamsFrom(sp["crTeams"], S.crTeams, S.nCrTeams, 6);
    if (sp["fbLeagues"].is<JsonArrayConst>()) {
      char lg[4][20];
      memset(lg, 0, sizeof(lg));
      uint8_t k = 0;
      for (JsonVariantConst v : sp["fbLeagues"].as<JsonArrayConst>()) {
        if (k >= 4) break;
        if (v.is<const char*>() && *v.as<const char*>()) safeCopy(lg[k++], v.as<const char*>(), 20);
      }
      if (k != S.nFbLeagues || memcmp(lg, S.fbLeagues, sizeof(lg))) { memcpy(S.fbLeagues, lg, sizeof(lg)); S.nFbLeagues = k; ch = true; }
    }
    ch |= setBool(sp, "crAll", S.crAll);
    ch |= setBool(sp, "alGoal", S.alGoal);
    ch |= setBool(sp, "alWkt", S.alWkt);
    ch |= setBool(sp, "alRes", S.alRes);
    ch |= setBool(sp, "alStart", S.alStart);
    ch |= setBool(sp, "alAny", S.alAny);
  }
  JsonObjectConst m = o["markets"];
  if (!m.isNull()) {
    ch |= setStr(m, "crypto", S.crypto, sizeof(S.crypto));
    ch |= setStr(m, "stocks", S.stocks, sizeof(S.stocks));
    ch |= setStr(m, "fx", S.fx, sizeof(S.fx));
    ch |= setStr(m, "vs", S.vs, sizeof(S.vs));
  }
  ch |= setStr(o, "news", S.news, sizeof(S.news));
  ch |= setStr(o, "ical", S.ical, sizeof(S.ical));
  JsonObjectConst so = o["sound"];
  if (!so.isNull()) {
    ch |= setBool(so, "on", S.sound);
    ch |= setNum<uint8_t>(so, "volume", S.volume, 0, 100);
    ch |= setHM(so, "quietFrom", S.quietFrom);
    ch |= setHM(so, "quietTo", S.quietTo);
    ch |= setNum<uint8_t>(so, "mask", S.sndMask, 0, 255);
    ch |= setBool(so, "hourly", S.hourly);
  }
  if (o["alarms"].is<JsonArrayConst>()) {
    Alarm al[MAX_ALARMS];
    memset(al, 0, sizeof(al));
    uint8_t k = 0;
    for (JsonObjectConst a : o["alarms"].as<JsonArrayConst>()) {
      if (k >= MAX_ALARMS) break;
      int hm = parseHM(a["time"] | "");
      if (hm < 0) continue;
      al[k].on = a["on"] | true;
      al[k].h = hm / 60;
      al[k].m = hm % 60;
      al[k].days = a["days"] | 0x7F;
      safeCopy(al[k].label, a["label"] | "", sizeof(al[k].label));
      k++;
    }
    if (k != S.nAlarms || memcmp(al, S.alarms, sizeof(al))) { memcpy(S.alarms, al, sizeof(al)); S.nAlarms = k; ch = true; }
  }
  JsonObjectConst in = o["input"];
  if (!in.isNull()) {
    ch |= setNum<uint16_t>(in, "multiMs", S.multiMs, 150, 600);
    ch |= setNum<uint16_t>(in, "holdMs", S.holdMs, 300, 2000);
    ch |= setNum<uint16_t>(in, "longMs", S.longMs, 1500, 8000);
  }
  ch |= setBool(o, "pcPresence", S.pcPresence);
  return ch;
}

void cardsToJson(JsonArray a) {
  for (int i = 0; i < gCardN; i++) {
    JsonObject o = a.add<JsonObject>();
    o["t"] = CARD_TYPES[gCards[i].type].key;
    o["on"] = gCards[i].on;
    o["dur"] = gCards[i].dur;
    o["arg"] = gCards[i].arg;
    o["name"] = gCards[i].name;
  }
}

bool cardsFromJson(JsonArrayConst a) {
  CardCfg tmp[MAX_CARDS];
  memset(tmp, 0, sizeof(tmp));
  uint8_t n = 0;
  for (JsonObjectConst o : a) {
    if (n >= MAX_CARDS) break;
    int t = cardTypeFromKey(o["t"] | "");
    if (t < 0) continue;
    tmp[n].type = t;
    tmp[n].on = o["on"] | true;
    tmp[n].dur = constrain((int)(o["dur"] | 0), 0, 240);
    safeCopy(tmp[n].arg, o["arg"] | "", sizeof(tmp[n].arg));
    safeCopy(tmp[n].name, o["name"] | "", sizeof(tmp[n].name));
    n++;
  }
  if (!n) return false;   // never allow an empty card list
  if (n == gCardN && !memcmp(tmp, gCards, sizeof(CardCfg) * n)) return false;
  memcpy(gCards, tmp, sizeof(tmp));
  gCardN = n;
  return true;
}

// ------------------------------------------------------------------ load / save
static void save() {
  JsonDocument d;
  settingsToJson(d["settings"].to<JsonObject>(), true);
  cardsToJson(d["cards"].to<JsonArray>());
  d["v"] = 2;
  String out;
  serializeJson(d, out);
  if (!saveFileAtomic("/settings.json", out.c_str(), out.length())) logf("[CFG] save failed");
  else logf("[CFG] saved (%u bytes)", (unsigned)out.length());
}

void settingsBegin() {
  if (!cfgMx) cfgMx = xSemaphoreCreateRecursiveMutex();
  settingsDefaults(S);
  cardsDefaults();
  String raw = readFile("/settings.json");
  if (!raw.length()) { logf("[CFG] first start, defaults"); save(); return; }
  JsonDocument d;
  DeserializationError err = deserializeJson(d, raw);
  if (err) { logf("[CFG] settings unreadable (%s), using defaults", err.c_str()); return; }
  settingsFromJson(d["settings"].as<JsonObjectConst>());
  if (d["cards"].is<JsonArrayConst>()) cardsFromJson(d["cards"].as<JsonArrayConst>());
  logf("[CFG] loaded, %u cards", gCardN);
}

void settingsMarkDirty() {
  dirtyAt = millis() | 1;
  gCfgRev = gCfgRev + 1;
}

// Debounced: dragging a slider in the dashboard writes flash once, not 50 times.
void settingsLoop() {
  if (dirtyAt && millis() - dirtyAt > 2000) {
    dirtyAt = 0;
    cfgLock();
    save();
    cfgUnlock();
  }
}

void settingsSaveNow() {
  if (!dirtyAt) return;
  dirtyAt = 0;
  cfgLock();
  save();
  cfgUnlock();
}

void settingsFactoryReset() {
  LittleFS.remove("/settings.json");
  LittleFS.remove("/settings.json.tmp");
}

bool cardEnabled(uint8_t type) {
  for (int i = 0; i < gCardN; i++) if (gCards[i].on && gCards[i].type == type) return true;
  return false;
}

static void csvTo(JsonArray a, const char* csv) {
  String s = csv;
  int start = 0;
  while (start <= (int)s.length()) {
    int c = s.indexOf(',', start);
    if (c < 0) c = s.length();
    String item = s.substring(start, c);
    item.trim();
    if (item.length()) a.add(item);
    start = c + 1;
  }
}

void buildSubscription(JsonDocument& d) {
  d["name"] = S.host;
  if (S.lat != 0 || S.lon != 0) {
    d["loc"]["lat"] = S.lat;
    d["loc"]["lon"] = S.lon;
  }
  d["units"] = S.imperial ? "imperial" : "metric";
  d["tz"] = S.tzName;
  d["wx"] = cardEnabled(CT_WEATHER) || cardEnabled(CT_FORECAST) || (S.reactWeather && cardEnabled(CT_EMO));
  d["aq"] = cardEnabled(CT_AIR);
  d["qt"] = cardEnabled(CT_QUOTE);
  bool fb = cardEnabled(CT_FOOTBALL);
  JsonObject f = d["fb"].to<JsonObject>();
  JsonArray lg = f["lg"].to<JsonArray>();
  JsonArray tm = f["tm"].to<JsonArray>();
  JsonArray tb = f["tb"].to<JsonArray>();
  if (fb) {
    for (int i = 0; i < S.nFbLeagues; i++) lg.add(S.fbLeagues[i]);
    for (int i = 0; i < gCardN; i++)
      if (gCards[i].on && gCards[i].type == CT_FOOTBALL && gCards[i].arg[0]) lg.add(gCards[i].arg);
    teamsTo(tm, S.fbTeams, S.nFbTeams);
  }
  for (int i = 0; i < gCardN; i++)
    if (gCards[i].on && gCards[i].type == CT_TABLE) tb.add(gCards[i].arg[0] ? gCards[i].arg : (S.nFbLeagues ? S.fbLeagues[0] : "eng.1"));
  JsonObject c = d["cr"].to<JsonObject>();
  if (cardEnabled(CT_CRICKET)) {
    teamsTo(c["tm"].to<JsonArray>(), S.crTeams, S.nCrTeams);
    c["all"] = S.crAll;
  }
  if (cardEnabled(CT_MARKETS)) {
    JsonObject m = d["mk"].to<JsonObject>();
    csvTo(m["cr"].to<JsonArray>(), S.crypto);
    csvTo(m["st"].to<JsonArray>(), S.stocks);
    csvTo(m["fx"].to<JsonArray>(), S.fx);
    m["vs"] = S.vs;
  }
  if (cardEnabled(CT_NEWS)) d["nw"] = S.news;
  if (cardEnabled(CT_AGENDA)) d["ic"] = S.ical;
  JsonObject al = d["al"].to<JsonObject>();
  al["goal"] = S.alGoal;
  al["wkt"] = S.alWkt;
  al["res"] = S.alRes;
  al["start"] = S.alStart;
  al["any"] = S.alAny;
}
