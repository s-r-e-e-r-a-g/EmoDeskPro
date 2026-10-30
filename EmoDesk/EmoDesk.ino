/*
  EmoDesk 2 - an Emo-style desktop companion
  ESP32-C3 Super Mini + SSD1306 128x64 + TTP223 touch (+ optional MAX98357A)

  Arduino IDE: board "ESP32C3 Dev Module", USB CDC On Boot: Enabled,
  Flash size 4MB, Partition scheme: "Minimal SPIFFS (1.9MB APP with OTA)"
  (partitions.csv in this folder is used automatically).
  Libraries: U8g2, ArduinoJson 7, ESPAsyncWebServer + AsyncTCP (by ESP32Async).
  See README.md.
*/
#include "config.h"
#include <WiFi.h>
#include <Wire.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <ESPAsyncWebServer.h>
#include "sys.h"
#include "settings.h"
#include "gfx.h"
#include "input.h"
#include "emo.h"
#include "audio.h"
#include "alerts.h"
#include "wifi_mgr.h"
#include "feed.h"
#include "cards.h"
#include "web.h"

static uint32_t lastFrame = 0, lastFeedV = 0, lastSecond = 0;
static bool forceFrame = true;
static char toastText[28] = "";
static uint32_t toastUntil = 0;
static bool swallowTouch = false;
static uint32_t swallowUntil = 0;
static Settings before;   // static: too big for the stack

static void toast(const char* t) {
  safeCopy(toastText, t, sizeof(toastText));
  toastUntil = millis() + 1400;
  forceFrame = true;
}

// ------------------------------------------------------------------ settings apply
static void applyEffects() {
  if (strcmp(before.tz, S.tz)) { setenv("TZ", S.tz, 1); tzset(); }
  if (before.flip != S.flip) gfxApplySettings();
  if (before.i2cHz != S.i2cHz) Wire.setClock(S.i2cHz);
  if (memcmp(before.wifi, S.wifi, sizeof(S.wifi)) || before.txPower != S.txPower || before.wifiSleep != S.wifiSleep || strcmp(before.host, S.host))
    wifiReconnect();
  if (before.sound != S.sound || before.volume != S.volume) audioApply();
  forceFrame = true;
}

static void applySettings(const String& js) {
  JsonDocument d;
  if (deserializeJson(d, js)) return;
  cfgLock();
  memcpy(&before, &S, sizeof(S));
  bool ch = settingsFromJson(d.as<JsonObjectConst>());
  JsonObjectConst add = d["wifiAdd"];
  if (!add.isNull()) {
    // setup page: new network goes first; an existing entry with that name is replaced
    WifiNet nets[MAX_WIFI];
    memset(nets, 0, sizeof(nets));
    safeCopy(nets[0].ssid, add["ssid"] | "", sizeof(nets[0].ssid));
    safeCopy(nets[0].pass, add["pass"] | "", sizeof(nets[0].pass));
    int k = 1;
    for (int i = 0; i < MAX_WIFI && k < MAX_WIFI; i++)
      if (S.wifi[i].ssid[0] && strcmp(S.wifi[i].ssid, nets[0].ssid)) nets[k++] = S.wifi[i];
    memcpy(S.wifi, nets, sizeof(nets));
    ch = true;
  }
  cfgUnlock();
  if (ch) {
    settingsMarkDirty();
    applyEffects();
    logf("[CFG] settings changed");
  }
}

static void applyCards(const String& js) {
  JsonDocument d;
  if (deserializeJson(d, js)) return;
  cfgLock();
  bool ch = cardsFromJson(d["cards"].as<JsonArrayConst>());
  cfgUnlock();
  if (ch) { settingsMarkDirty(); cardsChanged(); forceFrame = true; logf("[CFG] %u cards", gCardN); }
}

static void restore(const String& js) {
  JsonDocument d;
  if (deserializeJson(d, js)) return;
  cfgLock();
  memcpy(&before, &S, sizeof(S));
  settingsFromJson(d["settings"].as<JsonObjectConst>());
  if (d["cards"].is<JsonArrayConst>()) cardsFromJson(d["cards"].as<JsonArrayConst>());
  cfgUnlock();
  settingsMarkDirty();
  settingsSaveNow();
  cardsChanged();
  applyEffects();
  toast("Backup restored");
}

// ------------------------------------------------------------------ gestures
static void screenOn() { if (!gfxIsOn()) { gfxPower(true); forceFrame = true; } }

static void handleGesture(uint8_t g, uint8_t src) {
  uint32_t now = millis();
  if (swallowTouch) {                       // rest of a touch that woke the screen / left a game
    if (g == G_UP) { swallowTouch = false; swallowUntil = now + S.multiMs + 80; }
    return;
  }
  if (now < swallowUntil && g != G_DOWN) return;
  emoWake();
  if (!gfxIsOn()) {
    screenOn();
    if (g == G_DOWN) swallowTouch = true;
    return;
  }
  forceFrame = true;
  if (alertGesture(g)) { if (g == G_DOWN) swallowTouch = true; return; }

  if (src == SRC_BOOT) {
    if (g == G_TAP) cardsNext(1);
    else if (g == G_DOUBLE) cardsNext(-1);
    else if (g == G_HOLD) gfxPower(false);
    else if (g == G_LONG) { S.rotate = !S.rotate; settingsMarkDirty(); toast(S.rotate ? "Rotation on" : "Rotation off"); }
    return;
  }
  if (cardsCurrentRaw()) { cardsGesture(g); return; }   // games take every touch
  if (g == G_DOWN || g == G_UP) { cardsGesture(g); return; }

  switch (g) {
    case G_TAP: cardsNext(1); audioPlay("tap", SE_UI); break;
    case G_DOUBLE: if (!cardsGesture(G_DOUBLE)) cardsNext(-1); break;
    case G_TRIPLE: cardsHome(); break;
    case G_HOLD:
      if (!cardsGesture(G_HOLD)) { cardsPinToggle(); toast(cardsPinned() ? "Pinned" : "Unpinned"); }
      break;
    case G_LONG: gfxPower(false); swallowTouch = true; break;
  }
}

// ------------------------------------------------------------------ commands from web / net
static void handleCmd(const Cmd& c) {
  String js;
  switch (c.type) {
    case CMD_GESTURE: screenOn(); inputInject(c.a); break;
    case CMD_ACTION:
      screenOn();
      if (!strcmp(c.s, "next")) cardsNext(1);
      else if (!strcmp(c.s, "prev")) cardsNext(-1);
      else if (!strcmp(c.s, "home")) cardsHome();
      else if (!strcmp(c.s, "pin")) cardsPinToggle();
      else if (!strcmp(c.s, "goto")) cardsGoto(c.a);
      else if (!strcmp(c.s, "screen")) gfxPower(!gfxIsOn());
      else if (!strcmp(c.s, "rotate")) { S.rotate = !S.rotate; settingsMarkDirty(); }
      forceFrame = true;
      break;
    case CMD_MESSAGE:
      addMessage(c.s2, c.s);
      alertPush(AK_MSG, c.s2, c.s, 0, c.a);
      break;
    case CMD_NOTIFY: alertPush(AK_NOTIFY, c.s2, c.s, 0, c.b); break;
    case CMD_FEED_EVENT: alertPush(alertKindFromCode(c.a), c.s2, c.s, (int8_t)c.b); break;
    case CMD_HUB_CMD:
      if (!strncmp(c.s, "emo:", 4)) { int m = moodFromName(c.s + 4); if (m >= 0) { emoSet(m, 10000); cardsHome(); } }
      else if (!strcmp(c.s, "next")) cardsNext(1);
      else if (!strcmp(c.s, "prev")) cardsNext(-1);
      else if (!strcmp(c.s, "home")) cardsHome();
      screenOn();
      forceFrame = true;
      break;
    case CMD_EMO: { int m = moodFromName(c.s); if (m >= 0) emoSet(m, 10000); break; }
    case CMD_SOUND: audioPlay(c.s, SE_UI); break;
    case CMD_APPLY_SETTINGS: if (mailTake(MB_SETTINGS, js)) applySettings(js); break;
    case CMD_APPLY_CARDS: if (mailTake(MB_CARDS, js)) applyCards(js); break;
    case CMD_RESTORE: if (mailTake(MB_RESTORE, js)) restore(js); break;
    case CMD_REBOOT:
    case CMD_FACTORY:
      if (c.type == CMD_FACTORY) settingsFactoryReset();
      else settingsSaveNow();
      gfxClear();
      emoSet(M_SLEEP, 5000);
      u8g2.setFont(F_MB);
      textC(64, 36, c.type == CMD_FACTORY ? "Resetting..." : "Restarting...");
      gfxPresent();
      delay(400);
      ESP.restart();
      break;
  }
}

// ------------------------------------------------------------------ screen power & brightness
static void screenLoop() {
  uint32_t now = millis();
  int m = minuteOfDay();
  bool night = S.nightDim && m >= 0 && inWindow(m, parseHM(S.dimFrom), parseHM(S.dimTo));
  gfxContrast(night ? S.dimLevel : S.bright);
  if (alertActive()) { screenOn(); return; }
  uint32_t idle = now - inputLastMs();
  if (S.offAfterMin && gfxIsOn() && idle > S.offAfterMin * 60000UL) gfxPower(false);
  // PC companion presence: screen off while your PC is locked
  static bool pcOff = false;
  bool away = S.pcPresence && gPc.at && now - gPc.at < 30000 && gPc.locked;
  if (away && !pcOff && idle > 60000) { pcOff = true; gfxPower(false); }
  if (!away && pcOff) { pcOff = false; screenOn(); }
  ledSet(alertActive() && (now / 300) % 2);
}

// ------------------------------------------------------------------ special screens
static void drawSetup() {
  int page = (millis() / 7000) % 2;
  u8g2.setFont(F_MB);
  if (page == 0) {
    qrCode(3, 6, "WIFI:S:" SETUP_AP_SSID ";T:nopass;;", 2);
    u8g2.drawStr(60, 14, "Set me up");
    u8g2.setFont(F_S);
    u8g2.drawStr(60, 28, "1. Scan or join");
    u8g2.drawStr(60, 38, "Wi-Fi:");
    u8g2.setFont(F_XS);
    u8g2.drawStr(60, 48, SETUP_AP_SSID);
    u8g2.setFont(F_S);
    u8g2.drawStr(60, 60, "2. Open the page");
  } else {
    qrCode(3, 6, "http://192.168.4.1/", 2);
    u8g2.drawStr(60, 14, "Then open");
    u8g2.setFont(F_S);
    u8g2.drawStr(60, 28, "192.168.4.1");
    u8g2.drawStr(60, 42, "pick your");
    u8g2.drawStr(60, 52, "network and");
    u8g2.drawStr(60, 62, "password");
  }
}

static void drawToast() {
  if (!toastUntil || millis() > toastUntil) { toastUntil = 0; return; }
  u8g2.setFont(F_MB);
  int w = textW(toastText) + 12;
  int x = 64 - w / 2;
  u8g2.setDrawColor(0);
  u8g2.drawRBox(x - 1, 47, w + 2, 16, 4);
  u8g2.setDrawColor(1);
  u8g2.drawRBox(x, 48, w, 14, 4);
  u8g2.setDrawColor(0);
  u8g2.drawStr(x + 6, 59, toastText);
  u8g2.setDrawColor(1);
}

static void render() {
  gfxClear();
  if (wifiState() == WF_NO_CREDS && wifiApActive()) drawSetup();
  else if (alertActive()) alertDraw();
  else cardsDraw();
  drawToast();
  gfxPresent();
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  sysBegin();
  logf("EmoDesk %s, reset: %s%s", FW_VERSION, resetReasonStr(), gSafeMode ? " - SAFE MODE (3 crashes in a row)" : "");
  settingsBegin();
  setenv("TZ", S.tz, 1);
  tzset();
  gfxBegin();
  inputBegin();
  emoBegin();
  emoSet(M_HAPPY, 2500);
  // hello screen while Wi-Fi starts
  gfxClear();
  emoDraw(64, 26, 0.8f);
  u8g2.setFont(F_MB);
  textC(64, 60, gSafeMode ? "safe mode" : "waking up...");
  gfxPresent();
  audioBegin();
  audioPlay("boot", SE_UI);
  wifiBegin();
  netTaskBegin();
  cardsBegin();
  webBegin();
  enableLoopWDT();          // a frozen loop reboots instead of hanging forever
  logf("[SYS] ready, %u KB free", (unsigned)(ESP.getFreeHeap() / 1024));
}

void loop() {
  feedLoopWDT();
  inputPoll();
  uint8_t g, src;
  while (inputGet(g, src)) handleGesture(g, src);
  Cmd c;
  for (int i = 0; i < 6 && busTake(c); i++) handleCmd(c);

  wifiLoop();
  sysLoop();
  settingsLoop();
  uint32_t now = millis();
  if (now - lastSecond >= 1000) { lastSecond = now; alarmsLoop(); screenLoop(); }
  alertLoop();
  emoUpdate();
  cardsLoop();

  // leave a game by holding 1.5 s
  if (cardsCurrentRaw() && inputHeldMs() > 1500 && !swallowTouch) { cardsNext(1); swallowTouch = true; forceFrame = true; }

  // Frame pacing: each screen says how often it needs redrawing; a new feed
  // or any input redraws at once. Static cards cost ~nothing.
  uint32_t fv = feedVersion();
  if (fv != lastFeedV) { lastFeedV = fv; forceFrame = true; }
  uint32_t every = gfxAnimating() ? 16 : alertActive() ? alertFrameMs() : cardsFrameMs();
  if (toastUntil) every = min<uint32_t>(every, 100);
  if (gfxIsOn() && (forceFrame || now - lastFrame >= every)) {
    forceFrame = false;
    lastFrame = now;
    render();
  }
  webLoop();
  delay(gfxIsOn() ? 2 : 10);
}
