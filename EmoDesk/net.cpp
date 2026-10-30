// Network task: the hub client (long-poll) and the direct-weather fallback.
//
// One task, one request at a time, one reused keep-alive connection. With a
// hub, a single request carries every card's data and returns the moment
// something changes (goal, wicket, new headline), so live scores arrive within
// seconds instead of after a queue of 20+ separate HTTPS fetches.
#include "feed.h"
#include "sys.h"
#include "settings.h"
#include "wifi_mgr.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/semphr.h>

static Feed bufA, bufB;
static Feed* front = &bufA;
static Feed* back = &bufB;
static SemaphoreHandle_t feedMx = nullptr;
static volatile uint32_t swapCount = 0;

PcStats gPc;
Message gMsgs[4];
uint8_t gMsgN = 0;

static volatile HubState hState = HUB_OFF;
static char hErr[48] = "";
static volatile uint32_t hLastOk = 0;

void feedLock() { xSemaphoreTakeRecursive(feedMx, portMAX_DELAY); }
void feedUnlock() { xSemaphoreGiveRecursive(feedMx); }
const Feed& feed() { return *front; }
uint32_t feedVersion() { return swapCount; }
HubState hubState() { return hState; }
const char* hubError() { return hErr; }
uint32_t hubLastOkS() { return hLastOk; }

void addMessage(const char* from, const char* text) {
  // UI loop only
  for (int i = 3; i > 0; i--) gMsgs[i] = gMsgs[i - 1];
  safeCopy(gMsgs[0].from, from && *from ? from : "Message", sizeof(gMsgs[0].from));
  safeCopy(gMsgs[0].text, text, sizeof(gMsgs[0].text));
  gMsgs[0].at = millis() | 1;
  if (gMsgN < 4) gMsgN++;
}

bool feedHasLiveMine() {
  const Feed& f = *front;
  for (int i = 0; i < f.nfb; i++) if (f.fb[i].mine && f.fb[i].st == MS_IN) return true;
  for (int i = 0; i < f.ncr; i++) if (f.cr[i].mine && f.cr[i].st == MS_IN) return true;
  return false;
}

static void swapIn() {
  back->gotAt = millis();
  feedLock();
  Feed* t = front;
  front = back;
  back = t;
  swapCount = swapCount + 1;
  feedUnlock();
  // Start the next build from the current data, so sections the hub did not
  // send (or failed) keep their last good values.
  memcpy(back, front, sizeof(Feed));
}

static uint8_t stateOf(const char* s) {
  if (!s) return MS_PRE;
  if (s[0] == 'i') return MS_IN;
  if (s[0] == 'p' && s[1] == 'o') return MS_POST;
  return MS_PRE;
}
static int16_t x10(JsonVariantConst v) { return (int16_t)lroundf((v | 0.0f) * 10.0f); }

// ------------------------------------------------------------------ hub feed -> structs
static void parseFeed(JsonDocument& d, Feed& f) {
  f.v = d["v"] | 0;
  JsonObjectConst w = d["wx"];
  if (!w.isNull()) {
    Weather& x = f.wx;
    memset(&x, 0, sizeof(x));
    x.ok = true;
    x.t10 = x10(w["t"]); x.f10 = x10(w["f"]); x.wind10 = x10(w["w"]);
    x.hum = w["h"] | 0; x.code = w["c"] | 0; x.day = (w["d"] | 1) == 1;
    x.hi10 = x10(w["hi"]); x.lo10 = x10(w["lo"]); x.uv10 = (uint8_t)constrain((int)x10(w["uv"]), 0, 255);
    safeCopy(x.sr, w["sr"] | "", sizeof(x.sr));
    safeCopy(x.ss, w["ss"] | "", sizeof(x.ss));
    for (JsonArrayConst h : w["hr"].as<JsonArrayConst>()) {
      if (x.nh >= 12) break;
      WxHour& o = x.hr[x.nh++];
      o.h = h[0] | 0; o.t10 = x10(h[1]); o.code = h[2] | 0; o.pop = h[3] | 0;
    }
    for (JsonArrayConst dd : w["dy"].as<JsonArrayConst>()) {
      if (x.nd >= 6) break;
      WxDay& o = x.dy[x.nd++];
      o.wd = dd[0] | 0; o.code = dd[1] | 0; o.hi10 = x10(dd[2]); o.lo10 = x10(dd[3]); o.pop = dd[4] | 0;
    }
  }
  JsonObjectConst a = d["aq"];
  if (!a.isNull()) {
    f.aq.ok = true;
    f.aq.aqi = a["aqi"] | 0;
    f.aq.pm25x10 = x10(a["pm25"]);
    f.aq.pm10x10 = x10(a["pm10"]);
    f.aq.uv10 = x10(a["uv"]);
  }
  if (d["fb"].is<JsonArrayConst>()) {
    f.nfb = 0;
    for (JsonObjectConst m : d["fb"].as<JsonArrayConst>()) {
      if (f.nfb >= 14) break;
      FbMatch& o = f.fb[f.nfb++];
      memset(&o, 0, sizeof(o));
      safeCopy(o.id, m["id"] | "", sizeof(o.id));
      safeCopy(o.lg, m["lg"] | "", sizeof(o.lg));
      safeCopy(o.lid, m["lid"] | "", sizeof(o.lid));
      o.st = stateOf(m["s"]);
      safeCopy(o.d, m["d"] | "", sizeof(o.d));
      o.t = m["t"] | 0;
      o.mine = (m["m"] | 0) == 1;
      const char* sides[2] = {"h", "a"};
      FbSide* out[2] = {&o.h, &o.a};
      for (int i = 0; i < 2; i++) {
        JsonObjectConst s = m[sides[i]];
        safeCopy(out[i]->a, s["a"] | "?", sizeof(out[i]->a));
        safeCopy(out[i]->n, s["n"] | "", sizeof(out[i]->n));
        out[i]->s = s["s"] | -1;
        safeCopy(out[i]->id, s["id"] | "", sizeof(out[i]->id));
      }
      o.h.red = m["rh"] | 0;
      o.a.red = m["ra"] | 0;
      safeCopy(o.h.goals, m["gh"] | "", sizeof(o.h.goals));
      safeCopy(o.a.goals, m["ga"] | "", sizeof(o.a.goals));
    }
  }
  if (d["cr"].is<JsonArrayConst>()) {
    f.ncr = 0;
    for (JsonObjectConst m : d["cr"].as<JsonArrayConst>()) {
      if (f.ncr >= 6) break;
      CrMatch& o = f.cr[f.ncr++];
      memset(&o, 0, sizeof(o));
      safeCopy(o.id, m["id"] | "", sizeof(o.id));
      safeCopy(o.ser, m["ser"] | "", sizeof(o.ser));
      safeCopy(o.ti, m["ti"] | "", sizeof(o.ti));
      safeCopy(o.f, m["f"] | "", sizeof(o.f));
      o.st = stateOf(m["s"]);
      safeCopy(o.status, m["st"] | "", sizeof(o.status));
      o.t = m["t"] | 0;
      o.crr10 = x10(m["crr"]);
      o.rrr10 = x10(m["rrr"]);
      safeCopy(o.need, m["nd"] | "", sizeof(o.need));
      o.mine = (m["m"] | 0) == 1;
      int i = 0;
      for (JsonObjectConst s : m["a"].as<JsonArrayConst>()) {
        if (i >= 2) break;
        CrSide& c = o.s[i++];
        safeCopy(c.a, s["a"] | "?", sizeof(c.a));
        safeCopy(c.n, s["n"] | "", sizeof(c.n));
        safeCopy(c.sc, s["sc"] | "", sizeof(c.sc));
        safeCopy(c.ov, s["ov"] | "", sizeof(c.ov));
        c.bat = (s["b"] | 0) == 1;
        c.win = (s["w"] | 0) == 1;
      }
    }
  }
  if (d["tb"].is<JsonArrayConst>()) {
    f.ntb = 0;
    for (JsonObjectConst t : d["tb"].as<JsonArrayConst>()) {
      if (f.ntb >= 3) break;
      Table& o = f.tb[f.ntb++];
      memset(&o, 0, sizeof(o));
      safeCopy(o.lg, t["lg"] | "", sizeof(o.lg));
      safeCopy(o.lid, t["lid"] | "", sizeof(o.lid));
      for (JsonArrayConst r : t["r"].as<JsonArrayConst>()) {
        if (o.n >= 24) break;
        TableRow& x = o.r[o.n++];
        x.pos = r[0] | 0;
        safeCopy(x.a, r[1] | "?", sizeof(x.a));
        x.pts = r[2] | 0;
        x.gp = r[3] | 0;
        x.gd = r[4] | 0;
        x.mine = (r[5] | 0) == 1;
      }
    }
  }
  if (d["mk"].is<JsonArrayConst>()) {
    f.nmk = 0;
    for (JsonObjectConst m : d["mk"].as<JsonArrayConst>()) {
      if (f.nmk >= 16) break;
      Ticker& o = f.mk[f.nmk++];
      memset(&o, 0, sizeof(o));
      safeCopy(o.s, m["s"] | "?", sizeof(o.s));
      safeCopy(o.p, m["p"] | "", sizeof(o.p));
      o.c10 = x10(m["c"]);
      const char* k = m["k"] | "c";
      o.k = k[0];
      for (JsonVariantConst v : m["sp"].as<JsonArrayConst>()) {
        if (o.nsp >= 24) break;
        o.sp[o.nsp++] = (uint8_t)constrain((int)(v | 0), 0, 15);
      }
    }
  }
  if (d["nw"].is<JsonArrayConst>()) {
    f.nnw = 0;
    for (JsonObjectConst n : d["nw"].as<JsonArrayConst>()) {
      if (f.nnw >= 8) break;
      safeCopy(f.nw[f.nnw].h, n["h"] | "", sizeof(f.nw[0].h));
      safeCopy(f.nw[f.nnw].s, n["s"] | "", sizeof(f.nw[0].s));
      f.nnw++;
    }
  }
  if (d["qt"].is<JsonObjectConst>()) {
    safeCopy(f.qt.q, d["qt"]["q"] | "", sizeof(f.qt.q));
    safeCopy(f.qt.a, d["qt"]["a"] | "", sizeof(f.qt.a));
    f.hasQt = f.qt.q[0] != 0;
  }
  if (d["ic"].is<JsonArrayConst>()) {
    f.nev = 0;
    for (JsonObjectConst e : d["ic"].as<JsonArrayConst>()) {
      if (f.nev >= 8) break;
      CalEv& o = f.ev[f.nev++];
      o.t = e["t"] | 0;
      o.e = e["e"] | 0;
      o.ad = (e["ad"] | 0) == 1;
      safeCopy(o.d, e["d"] | "", sizeof(o.d));
    }
  }
}

// Alerts / messages / commands. Each has a sequence number so it is shown
// once. After a device or hub restart, only events from the last 2 minutes count.
static uint32_t evBoot = 0, evLastQ = 0;
static bool evPrimed = false;

static void handleEvents(JsonDocument& d) {
  uint32_t boot = d["b"] | 0;
  uint32_t serverNow = d["t"] | 0;
  if (boot != evBoot) { evBoot = boot; evLastQ = 0; evPrimed = false; }
  for (JsonObjectConst e : d["ev"].as<JsonArrayConst>()) {
    uint32_t q = e["q"] | 0;
    uint32_t ts = e["ts"] | 0;
    if (q <= evLastQ) continue;
    evLastQ = q;
    if (!evPrimed && serverNow && ts + 120 < serverNow) continue;
    const char* k = e["k"] | "";
    const char* ti = e["ti"] | "";
    const char* tx = e["tx"] | "";
    int mood = e["m"] | 0;
    if (!strcmp(k, "msg")) busPost(CMD_MESSAGE, tx, ti, 0);
    else if (!strcmp(k, "cmd")) busPost(CMD_HUB_CMD, e["c"] | "");
    else busPost(CMD_FEED_EVENT, tx, ti, (int16_t)(k[0] | (k[1] << 8)), mood);
  }
  evPrimed = true;
}

// ------------------------------------------------------------------ HTTP
static WiFiClient plainClient;
static WiFiClientSecure* tlsClient = nullptr;
static HTTPClient http;
static char subId[16] = "";
static uint32_t subRev = 0;
static uint32_t subHash = 0;

static NetworkClient& clientFor(const String& url) {
  if (url.startsWith("https")) {
    if (!tlsClient) {
      tlsClient = new WiFiClientSecure();
      tlsClient->setInsecure();   // hub token protects the data; LAN http needs no TLS at all
    }
    return *tlsClient;
  }
  return plainClient;
}

static void setErr(const char* e) {
  safeCopy(hErr, e, sizeof(hErr));
}

static uint32_t fnv(const String& s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  return h;
}

static bool hubRegister(const String& base, const String& token) {
  String body;
  {
    JsonDocument d;
    cfgLock();
    buildSubscription(d);
    subRev = gCfgRev;
    cfgUnlock();
    serializeJson(d, body);
  }
  uint32_t h = fnv(body);
  String url = base + "/v1/sub";
  http.begin(clientFor(url), url);
  http.setConnectTimeout(5000);
  http.setTimeout(12000);
  http.addHeader("Content-Type", "application/json");
  if (token.length()) http.addHeader("X-Hub-Token", token);
  int code = http.POST(body);
  bool ok = false;
  if (code == 200) {
    JsonDocument r;
    if (!deserializeJson(r, http.getStream())) {
      safeCopy(subId, r["id"] | "", sizeof(subId));
      ok = subId[0] != 0;
    }
  } else if (code == 401) setErr("hub token rejected");
  else if (code < 0) setErr("hub unreachable");
  else { char b[32]; snprintf(b, sizeof(b), "hub error %d", code); setErr(b); }
  http.end();
  if (ok) {
    subHash = h;
    memset(back, 0, sizeof(Feed));   // sections no longer subscribed must not linger
    logf("[HUB] subscribed as %s (%u bytes)", subId, (unsigned)body.length());
  }
  return ok;
}

// Returns: 1 got data, 0 no change, -1 error, -2 re-register needed
static int hubPoll(const String& base, const String& token, uint32_t since) {
  char q[96];
  snprintf(q, sizeof(q), "/v1/feed?sub=%s&v=%lu&wait=%d", subId, (unsigned long)since, since ? 20 : 0);
  String url = base + q;
  http.begin(clientFor(url), url);
  http.setConnectTimeout(5000);
  http.setTimeout(28000);    // the hub holds the request up to 20 s
  if (token.length()) http.addHeader("X-Hub-Token", token);
  int code = http.GET();
  int ret = -1;
  if (code == 200) {
    JsonDocument d;
    DeserializationError e = deserializeJson(d, http.getStream());
    if (e) { logf("[HUB] bad feed JSON: %s", e.c_str()); setErr("bad data from hub"); }
    else {
      parseFeed(d, *back);
      handleEvents(d);
      swapIn();
      ret = 1;
    }
  } else if (code == 304) ret = 0;
  else if (code == 404) ret = -2;
  else if (code == 401) setErr("hub token rejected");
  else if (code < 0) setErr(code == HTTPC_ERROR_READ_TIMEOUT ? "hub timed out" : "hub unreachable");
  else { char b[32]; snprintf(b, sizeof(b), "hub error %d", code); setErr(b); }
  http.end();
  return ret;
}

// ------------------------------------------------------------------ direct weather
// Without a hub the device still shows weather (Open-Meteo, small filtered JSON).
static bool directWeather() {
  float lat, lon;
  bool imp;
  cfgLock();
  lat = S.lat; lon = S.lon; imp = S.imperial;
  cfgUnlock();
  if (lat == 0 && lon == 0) return false;
  char url[400];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,is_day"
           "&hourly=temperature_2m,weather_code&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
           "&timezone=auto&forecast_days=5&forecast_hours=13%s",
           lat, lon, imp ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");
  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient h;
  h.begin(c, url);
  h.setTimeout(10000);
  h.useHTTP10(true);
  int code = h.GET();
  bool ok = false;
  if (code == 200) {
    JsonDocument d;
    if (!deserializeJson(d, h.getStream())) {
      Weather& x = back->wx;
      memset(&x, 0, sizeof(x));
      JsonObjectConst cu = d["current"];
      x.ok = true;
      x.t10 = x10(cu["temperature_2m"]); x.f10 = x10(cu["apparent_temperature"]); x.wind10 = x10(cu["wind_speed_10m"]);
      x.hum = cu["relative_humidity_2m"] | 0; x.code = cu["weather_code"] | 0; x.day = (cu["is_day"] | 1) == 1;
      JsonObjectConst hr = d["hourly"];
      for (size_t i = 1; i < hr["time"].size() && x.nh < 12; i++) {
        WxHour& o = x.hr[x.nh++];
        const char* t = hr["time"][i] | "0000-00-00T00:00";
        o.h = strlen(t) > 12 ? atoi(t + 11) : 0;
        o.t10 = x10(hr["temperature_2m"][i]);
        o.code = hr["weather_code"][i] | 0;
      }
      JsonObjectConst dy = d["daily"];
      for (size_t i = 0; i < dy["time"].size() && x.nd < 6; i++) {
        WxDay& o = x.dy[x.nd++];
        int Y, M, D;
        if (sscanf(dy["time"][i] | "", "%d-%d-%d", &Y, &M, &D) == 3) {
          struct tm tt = {};
          tt.tm_year = Y - 1900; tt.tm_mon = M - 1; tt.tm_mday = D; tt.tm_hour = 12;
          time_t ts = mktime(&tt);
          struct tm out;
          localtime_r(&ts, &out);
          o.wd = out.tm_wday;
        }
        o.code = dy["weather_code"][i] | 0;
        o.hi10 = x10(dy["temperature_2m_max"][i]);
        o.lo10 = x10(dy["temperature_2m_min"][i]);
      }
      x.hi10 = x.nd ? x.dy[0].hi10 : 0;
      x.lo10 = x.nd ? x.dy[0].lo10 : 0;
      const char* sr = dy["sunrise"][0] | "";
      const char* ss = dy["sunset"][0] | "";
      if (strlen(sr) >= 16) safeCopy(x.sr, sr + 11, 6);
      if (strlen(ss) >= 16) safeCopy(x.ss, ss + 11, 6);
      swapIn();
      ok = true;
    }
  }
  h.end();
  if (!ok) logf("[NET] weather fetch failed (%d)", code);
  return ok;
}

// ------------------------------------------------------------------ task
static void netTask(void*) {
  uint32_t backoff = 0, lastVer = 0, nextWeather = 0;
  int failsInRow = 0;
  vTaskDelay(pdMS_TO_TICKS(1500));
  for (;;) {
    if (!wifiOnline()) {
      if (hState != HUB_OFF) hState = HUB_CONNECTING;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    String base, token;
    cfgLock();
    base = S.hubUrl;
    token = S.hubToken;
    cfgUnlock();

    if (!base.length() || gSafeMode) {
      hState = HUB_OFF;
      if ((int32_t)(millis() - nextWeather) >= 0) {
        nextWeather = millis() + (directWeather() ? 15 * 60000UL : 60000UL);
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (backoff) { vTaskDelay(pdMS_TO_TICKS(backoff)); backoff = 0; }

    // (Re)subscribe when settings that matter changed.
    if (subRev != gCfgRev) {
      JsonDocument d;
      cfgLock();
      buildSubscription(d);
      uint32_t rev = gCfgRev;
      cfgUnlock();
      String body;
      serializeJson(d, body);
      if (fnv(body) != subHash) subId[0] = 0;
      subRev = rev;
    }
    if (!subId[0]) {
      if (hState != HUB_OK) hState = HUB_CONNECTING;
      if (!hubRegister(base, token)) {
        failsInRow++;
        hState = failsInRow >= 2 ? HUB_ERROR : HUB_CONNECTING;
        backoff = min<uint32_t>(60000, 2000UL << min(failsInRow, 5));
        continue;
      }
      lastVer = 0;
    }
    int r = hubPoll(base, token, lastVer);
    if (r == 1) {
      lastVer = front->v;
      failsInRow = 0;
      hState = HUB_OK;
      hErr[0] = 0;
      hLastOk = uptimeS();
    } else if (r == 0) {
      failsInRow = 0;
      hState = HUB_OK;
      hLastOk = uptimeS();
    } else if (r == -2) {
      subId[0] = 0;
    } else {
      failsInRow++;
      if (failsInRow >= 2) hState = HUB_ERROR;
      backoff = min<uint32_t>(60000, 1000UL << min(failsInRow, 6));
      logf("[HUB] poll failed (%s), retry in %lus", hErr, (unsigned long)(backoff / 1000));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void netTaskBegin() {
  if (!feedMx) feedMx = xSemaphoreCreateRecursiveMutex();
  memset(&bufA, 0, sizeof(bufA));
  memset(&bufB, 0, sizeof(bufB));
  memset(&gPc, 0, sizeof(gPc));
  http.setReuse(true);
  xTaskCreate(netTask, "net", NET_STACK, nullptr, NET_PRIO, nullptr);
}
