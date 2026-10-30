#include "web.h"
#include "sys.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "feed.h"
#include "gfx.h"
#include "cards.h"
#include "emo.h"
#include "input.h"
#include "audio.h"
#include "web_ui.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static uint32_t lastFramePush = 0, lastStatusPush = 0, pushedSeq = 0;
static volatile bool wsNewClient = false;
static volatile bool otaOk = false;
static String otaErr;

// Snapshot of UI-owned values the web task reports (written by the UI loop).
static volatile int snapCard = 0;
static char snapTitle[20] = "";
static volatile uint8_t snapMood = 0;
static volatile bool snapPinned = false;

// ------------------------------------------------------------------ helpers
static bool authed(AsyncWebServerRequest* r) {
  if (!S.pin[0]) return true;
  if (r->hasHeader("X-Emo-Pin") && r->header("X-Emo-Pin") == S.pin) return true;
  if (r->hasParam("pin") && r->getParam("pin")->value() == S.pin) return true;
  if (r->hasHeader("Cookie")) {
    String c = r->header("Cookie");
    int i = c.indexOf("emo_pin=");
    if (i >= 0) {
      int e = c.indexOf(';', i);
      if (c.substring(i + 8, e < 0 ? c.length() : e) == S.pin) return true;
    }
  }
  return false;
}

static void sendJson(AsyncWebServerRequest* r, const JsonDocument& d, int code = 200) {
  String out;
  serializeJson(d, out);
  AsyncWebServerResponse* res = r->beginResponse(code, "application/json", out);
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}
static void ok(AsyncWebServerRequest* r) { r->send(200, "application/json", "{\"ok\":true}"); }
static void fail(AsyncWebServerRequest* r, int code, const char* msg) {
  JsonDocument d;
  d["ok"] = false;
  d["error"] = msg;
  sendJson(r, d, code);
}

// Collect a request body into _tempObject (bounded).
static void collect(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
  if (total > 16384) return;
  if (!index) {
    r->_tempObject = malloc(total + 1);
    if (!r->_tempObject) return;
  }
  if (!r->_tempObject) return;
  memcpy((uint8_t*)r->_tempObject + index, data, len);
  if (index + len == total) ((char*)r->_tempObject)[total] = 0;
}
static const char* body(AsyncWebServerRequest* r) { return r->_tempObject ? (const char*)r->_tempObject : ""; }

static bool parseBody(AsyncWebServerRequest* r, JsonDocument& d) {
  if (!r->_tempObject) { fail(r, 400, "empty or too large body"); return false; }
  if (deserializeJson(d, body(r))) { fail(r, 400, "invalid JSON"); return false; }
  return true;
}

#define AUTH() do { if (!authed(r)) { fail(r, 401, "pin required"); return; } } while (0)

// ------------------------------------------------------------------ status
void webStatusJson(String& out) {
  JsonDocument d;
  d["fw"] = FW_VERSION;
  d["host"] = S.host;
  d["mode"] = wifiOnline() ? "online" : (wifiState() == WF_NO_CREDS ? "setup" : "connecting");
  d["ap"] = wifiApActive();
  d["ssid"] = wifiSsid();
  d["ip"] = wifiIp();
  d["rssi"] = wifiRssi();
  d["wifiReason"] = wifiLastReason();
  JsonObject h = d["hub"].to<JsonObject>();
  h["url"] = S.hubUrl[0] != 0;
  HubState hs = hubState();
  h["state"] = hs == HUB_OK ? "ok" : hs == HUB_ERROR ? "error" : hs == HUB_CONNECTING ? "connecting" : "off";
  h["error"] = hubError();
  h["lastOk"] = hubLastOkS() ? (int32_t)(uptimeS() - hubLastOkS()) : -1;
  d["uptime"] = uptimeS();
  d["heap"] = ESP.getFreeHeap();
  d["minHeap"] = ESP.getMinFreeHeap();
  d["block"] = heapLargest();
  d["card"] = snapCard;
  d["cardTitle"] = snapTitle;
  d["pinned"] = (bool)snapPinned;
  d["mood"] = moodName(snapMood);
  d["time"] = (uint32_t)time(nullptr);
  d["timeOk"] = timeValid();
  d["feedV"] = feedVersion();
  d["safe"] = gSafeMode;
  d["reset"] = resetReasonStr();
  d["ota"] = esp_ota_get_next_update_partition(nullptr) != nullptr;
  d["pin"] = S.pin[0] != 0;
  serializeJson(d, out);
}

static void wsEvent(AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    // the upgrade request carries the dashboard's PIN cookie
    if (S.pin[0] && !authed((AsyncWebServerRequest*)arg)) { c->close(4001, "pin required"); return; }
    wsNewClient = true;
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (!info->final || info->index || info->len != len || info->opcode != WS_TEXT || len > 64) return;
    char m[65];
    memcpy(m, data, len);
    m[len] = 0;
    // "tap" | "double" | "triple" | "hold" | "long" | "down" | "up"
    for (uint8_t g = G_TAP; g <= G_UP; g++) if (!strcmp(m, gestureName(g))) { busPost(CMD_GESTURE, nullptr, nullptr, g); break; }
  }
}

// ------------------------------------------------------------------ routes
void webBegin() {
  ws.onEvent(wsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (r->hasHeader("If-None-Match") && r->header("If-None-Match") == DASH_ETAG) { r->send(304); return; }
    AsyncWebServerResponse* res = r->beginResponse(200, "text/html", DASH_GZ, DASH_GZ_LEN);
    res->addHeader("Content-Encoding", "gzip");
    res->addHeader("ETag", DASH_ETAG);
    res->addHeader("Cache-Control", "no-cache");
    r->send(res);
  });

  // captive portal probes (Android, iOS, Windows) -> setup page
  for (const char* p : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/ncsi.txt", "/connecttest.txt", "/fwlink", "/success.txt", "/canonical.html"}) {
    server.on(p, HTTP_GET, [](AsyncWebServerRequest* r) {
      if (wifiApActive()) r->redirect("http://192.168.4.1/");
      else r->send(204);
    });
  }

  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* r) {
    String s;
    webStatusJson(s);
    r->send(200, "application/json", s);
  });

  server.on("/api/pin", HTTP_POST, [](AsyncWebServerRequest* r) {
    JsonDocument d;
    if (!parseBody(r, d)) return;
    String p = d["pin"] | "";
    if (S.pin[0] && p != S.pin) { fail(r, 401, "wrong pin"); return; }
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", "{\"ok\":true}");
    res->addHeader("Set-Cookie", "emo_pin=" + p + "; Path=/; Max-Age=31536000; SameSite=Strict");
    r->send(res);
  }, nullptr, collect);

  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* r) {
    AUTH();
    JsonDocument d;
    cfgLock();
    settingsToJson(d.to<JsonObject>(), false);
    cfgUnlock();
    sendJson(r, d);
  });
  server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest* r) {
    AUTH();
    JsonDocument d;
    if (!parseBody(r, d)) return;
    mailPut(MB_SETTINGS, body(r), strlen(body(r)));
    busPost(CMD_APPLY_SETTINGS);
    ok(r);
  }, nullptr, collect);

  server.on("/api/cards", HTTP_GET, [](AsyncWebServerRequest* r) {
    AUTH();
    JsonDocument d;
    cfgLock();
    cardsToJson(d["cards"].to<JsonArray>());
    cfgUnlock();
    JsonArray types = d["types"].to<JsonArray>();
    for (int i = 0; i < CT_COUNT; i++) {
      JsonObject t = types.add<JsonObject>();
      t["key"] = CARD_TYPES[i].key;
      t["title"] = CARD_TYPES[i].title;
      t["desc"] = CARD_TYPES[i].desc;
      t["arg"] = CARD_TYPES[i].argHelp;
      t["hub"] = CARD_TYPES[i].needsHub;
    }
    d["max"] = MAX_CARDS;
    sendJson(r, d);
  });
  server.on("/api/cards", HTTP_POST, [](AsyncWebServerRequest* r) {
    AUTH();
    JsonDocument d;
    if (!parseBody(r, d)) return;
    if (!d["cards"].is<JsonArray>() || !d["cards"].size()) { fail(r, 400, "cards list required"); return; }
    mailPut(MB_CARDS, body(r), strlen(body(r)));
    busPost(CMD_APPLY_CARDS);
    ok(r);
  }, nullptr, collect);

  server.on("/api/action", HTTP_POST, [](AsyncWebServerRequest* r) {
    AUTH();
    String a = r->hasParam("a") ? r->getParam("a")->value() : "";
    int i = r->hasParam("i") ? r->getParam("i")->value().toInt() : 0;
    if (!a.length()) { fail(r, 400, "a= required"); return; }
    busPost(CMD_ACTION, a.c_str(), nullptr, i);
    ok(r);
  });

  server.on("/api/message", HTTP_POST, [](AsyncWebServerRequest* r) {
    AUTH();
    JsonDocument d;
    if (!parseBody(r, d)) return;
    const char* t = d["text"] | "";
    if (!*t) { fail(r, 400, "text required"); return; }
    busPost(CMD_MESSAGE, t, d["from"] | "Dashboard", d["secs"] | 0);
    ok(r);
  }, nullptr, collect);

  server.on("/api/notify", HTTP_POST, [](AsyncWebServerRequest* r) {
    AUTH();
    JsonDocument d;
    if (!parseBody(r, d)) return;
    busPost(CMD_NOTIFY, d["text"] | "", d["title"] | "Notice", 0, d["secs"] | 0);
    ok(r);
  }, nullptr, collect);

  server.on("/api/emo", HTTP_POST, [](AsyncWebServerRequest* r) {
    AUTH();
    JsonDocument d;
    if (!parseBody(r, d)) return;
    if (moodFromName(d["mood"] | "") < 0) { fail(r, 400, "unknown mood"); return; }
    busPost(CMD_EMO, d["mood"] | "");
    ok(r);
  }, nullptr, collect);

  server.on("/api/sound", HTTP_POST, [](AsyncWebServerRequest* r) {
    AUTH();
    JsonDocument d;
    if (!parseBody(r, d)) return;
    busPost(CMD_SOUND, d["name"] | "");
    ok(r);
  }, nullptr, collect);

  // PC companion: CPU/RAM/GPU and now playing. Written directly (plain values).
  server.on("/api/pc", HTTP_POST, [](AsyncWebServerRequest* r) {
    AUTH();
    JsonDocument d;
    if (!parseBody(r, d)) return;
    gPc.cpu = d["cpu"] | 0.0f;
    gPc.ram = d["ram"] | 0.0f;
    gPc.gpu = d["gpu"] | 0.0f;
    gPc.cpuT = d["cpuT"] | 0.0f;
    gPc.gpuT = d["gpuT"] | 0.0f;
    safeCopy(gPc.song, d["song"] | "", sizeof(gPc.song));
    safeCopy(gPc.artist, d["artist"] | "", sizeof(gPc.artist));
    gPc.playing = d["playing"] | false;
    gPc.idleS = d["idle"] | 0;
    gPc.locked = d["locked"] | false;
    gPc.at = millis() | 1;
    ok(r);
  }, nullptr, collect);

  server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest* r) {
    String j;
    int n = wifiScanResults(j);
    if (n == -2) { wifiScanStart(); r->send(202, "application/json", "{\"scanning\":true}"); return; }
    if (n == -1) { r->send(202, "application/json", "{\"scanning\":true}"); return; }
    r->send(200, "application/json", j);
  });

  // Setup page: add a network (allowed without PIN only while in setup mode)
  server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (wifiState() != WF_NO_CREDS && !wifiApActive()) AUTH();
    JsonDocument d;
    if (!parseBody(r, d)) return;
    const char* ssid = d["ssid"] | "";
    if (!*ssid) { fail(r, 400, "ssid required"); return; }
    String pass = d["pass"] | "";
    JsonDocument w;
    w["wifiAdd"]["ssid"] = ssid;
    w["wifiAdd"]["pass"] = pass;
    String out;
    serializeJson(w, out);
    mailPut(MB_SETTINGS, out.c_str(), out.length());
    busPost(CMD_APPLY_SETTINGS);
    ok(r);
  }, nullptr, collect);

  server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest* r) {
    AUTH();
    char* buf = (char*)malloc(4200);
    if (!buf) { fail(r, 500, "no memory"); return; }
    logCopy(buf, 4200);
    r->send(200, "text/plain", buf);
    free(buf);
  });

  server.on("/api/backup", HTTP_GET, [](AsyncWebServerRequest* r) {
    AUTH();
    bool secrets = r->hasParam("secrets") && r->getParam("secrets")->value() == "1";
    JsonDocument d;
    d["emodesk"] = FW_VERSION;
    cfgLock();
    settingsToJson(d["settings"].to<JsonObject>(), secrets);
    cardsToJson(d["cards"].to<JsonArray>());
    cfgUnlock();
    String out;
    serializeJsonPretty(d, out);
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", out);
    res->addHeader("Content-Disposition", "attachment; filename=emodesk-backup.json");
    r->send(res);
  });
  server.on("/api/restore", HTTP_POST, [](AsyncWebServerRequest* r) {
    AUTH();
    JsonDocument d;
    if (!parseBody(r, d)) return;
    if (!d["settings"].is<JsonObject>()) { fail(r, 400, "not an EmoDesk backup"); return; }
    mailPut(MB_RESTORE, body(r), strlen(body(r)));
    busPost(CMD_RESTORE);
    ok(r);
  }, nullptr, collect);

  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* r) { AUTH(); busPost(CMD_REBOOT); ok(r); });
  server.on("/api/factory", HTTP_POST, [](AsyncWebServerRequest* r) { AUTH(); busPost(CMD_FACTORY); ok(r); });

  // Firmware update (dual OTA slots, the bootloader rolls back a bad image)
  server.on("/api/update", HTTP_POST,
    [](AsyncWebServerRequest* r) {
      if (!authed(r)) { fail(r, 401, "pin required"); return; }
      if (otaOk) { ok(r); busPost(CMD_REBOOT); }
      else fail(r, 500, otaErr.length() ? otaErr.c_str() : "update failed");
    },
    [](AsyncWebServerRequest* r, const String& fn, size_t index, uint8_t* data, size_t len, bool final) {
      if (!authed(r)) return;
      if (!index) {
        otaOk = false;
        otaErr = "";
        logf("[OTA] receiving %s", fn.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { otaErr = Update.errorString(); return; }
      }
      if (Update.isRunning() && Update.write(data, len) != len) otaErr = Update.errorString();
      if (final && Update.isRunning()) {
        if (Update.end(true) && !otaErr.length()) { otaOk = true; logf("[OTA] %u bytes ok", (unsigned)(index + len)); }
        else otaErr = Update.errorString();
      }
    });

  server.onNotFound([](AsyncWebServerRequest* r) {
    if (r->method() == HTTP_OPTIONS) { r->send(204); return; }
    if (wifiApActive() && !r->url().startsWith("/api/")) { r->redirect("http://192.168.4.1/"); return; }
    fail(r, 404, "not found");
  });

  server.begin();
  logf("[WEB] dashboard ready");
}

// ------------------------------------------------------------------ push to dashboards
void webLoop() {
  snapCard = cardsCurrent();
  safeCopy(snapTitle, cardsCurrentTitle(), sizeof(snapTitle));
  snapMood = emoMood();
  snapPinned = cardsPinned();
  uint32_t now = millis();
  if (now - lastStatusPush > 1000) ws.cleanupClients(3);
  if (!ws.count()) return;
  bool fresh = wsNewClient;
  // live mirror: up to 12 fps, only when the picture changed
  if ((fresh || gfxFrameSeq() != pushedSeq) && now - lastFramePush >= 80 && ws.availableForWriteAll()) {
    ws.binaryAll(gfxFrame(), 1024);
    pushedSeq = gfxFrameSeq();
    lastFramePush = now;
  }
  if (fresh || now - lastStatusPush > 2000) {
    wsNewClient = false;
    lastStatusPush = now;
    if (ws.availableForWriteAll()) {
      String s;
      webStatusJson(s);
      ws.textAll(s);
    }
  }
}
