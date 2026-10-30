// Data cards: weather, forecast, air, markets, news, quote, agenda, PC,
// messages and system.
#include "cards.h"
#include "gfx.h"
#include "sys.h"
#include "wifi_mgr.h"
#include "input.h"
#include <WiFi.h>
#include <time.h>

static uint16_t slow(CardCtx&) { return 1000; }

static void degC(int x, int y) { degree(x, y); }

// ================================================================== Weather
static bool wx_has(CardCtx& c) { return c.f.wx.ok; }
static int wx_pages(CardCtx& c) { return c.f.wx.nh >= 6 ? 2 : 1; }

static void wx_now(CardCtx& c) {
  const Weather& w = c.f.wx;
  wxIcon(19, 32, 1, w.code, w.day);
  char b[8];
  fmtTemp(b, sizeof(b), w.t10);
  u8g2.setFont(F_N32);
  int tw = textW(b);
  int x = 62 - tw / 2;
  if (x < 38) x = 38;
  u8g2.drawStr(x, 47, b);
  degC(x + tw + 1, 15);
  u8g2.setFont(F_S);
  char l[16];
  fmtTemp(b, sizeof(b), w.hi10);
  snprintf(l, sizeof(l), "H %s", b);
  u8g2.drawStr(94, 22, l);
  fmtTemp(b, sizeof(b), w.lo10);
  snprintf(l, sizeof(l), "L %s", b);
  u8g2.drawStr(94, 32, l);
  snprintf(l, sizeof(l), "%u%%", w.hum);
  u8g2.drawStr(94, 42, l);
  // tiny drop next to humidity
  u8g2.drawTriangle(122, 35, 120, 39, 124, 39);
  u8g2.drawDisc(122, 40, 2);
  char line[48];
  fmtTemp(b, sizeof(b), w.f10);
  snprintf(line, sizeof(line), "%s, feels %s", wxText(w.code), b);
  u8g2.setFont(F_MB);
  textFit(0, 61, 128, line);
}

static void wx_hours(CardCtx& c) {
  const Weather& w = c.f.wx;
  int n = min<int>(w.nh, 12);
  int lo = 9999, hi = -9999;
  for (int i = 0; i < n; i++) { lo = min<int>(lo, w.hr[i].t10); hi = max<int>(hi, w.hr[i].t10); }
  if (hi - lo < 30) { hi = lo + 30; }
  const int top = 22, bot = 50, x0 = 6, x1 = 121;
  int px = 0, py = 0;
  for (int i = 0; i < n; i++) {
    int x = x0 + i * (x1 - x0) / (n - 1);
    int y = bot - (w.hr[i].t10 - lo) * (bot - top) / (hi - lo);
    if (i) u8g2.drawLine(px, py, x, y);
    u8g2.drawDisc(x, y, 1);
    if (i % 3 == 0) {
      char b[8];
      fmtTemp(b, sizeof(b), w.hr[i].t10);
      u8g2.setFont(F_XS);
      textC(x, y - 4, b);
      char h[6];
      int hh = w.hr[i].h;
      if (S.h24) snprintf(h, sizeof(h), "%02d", hh);
      else snprintf(h, sizeof(h), "%d%s", hh % 12 ? hh % 12 : 12, hh < 12 ? "a" : "p");
      textC(x, 62, h);
      if (w.hr[i].pop >= 30) { snprintf(b, sizeof(b), "%d%%", w.hr[i].pop); textC(x, 56, b); }
    }
    px = x;
    py = y;
  }
}

static void wx_draw(CardCtx& c) {
  int pages = wx_pages(c);
  int p = autoPage(c, pages, 6000);
  header(p == 0 ? (S.place[0] ? S.place : "Weather") : "Next hours", pages, p);
  if (!c.f.wx.ok) { emptyState("No weather yet", S.lat == 0 && S.lon == 0 ? "Set your location in the dashboard." : "Waiting for data..."); return; }
  if (p == 0) wx_now(c); else wx_hours(c);
}
static bool wx_gesture(CardCtx& c, uint8_t g) {
  if (g != G_DOUBLE) return false;
  c.rt.page = (c.rt.page + 1) % wx_pages(c);
  c.rt.pageAt = millis();
  return true;
}
const CardImpl CI_WEATHER = {wx_draw, wx_gesture, wx_has, slow, nullptr, wx_pages, nullptr, false};

// ================================================================== Forecast
static bool fc_has(CardCtx& c) { return c.f.wx.ok && c.f.wx.nd >= 2; }
static void fc_draw(CardCtx& c) {
  header(cardTitle(c.idx));
  const Weather& w = c.f.wx;
  if (!fc_has(c)) { emptyState("No forecast yet"); return; }
  int n = min<int>(w.nd, 5);
  int cw = 128 / n;
  for (int i = 0; i < n; i++) {
    int cx = i * cw + cw / 2;
    u8g2.setFont(F_S);
    textC(cx, 20, i == 0 ? "Today" : WDAY3[w.dy[i].wd % 7]);
    wxIcon(cx, 31, 0, w.dy[i].code, true);
    char b[8];
    fmtTemp(b, sizeof(b), w.dy[i].hi10);
    u8g2.setFont(F_MB);
    textC(cx, 52, b);
    fmtTemp(b, sizeof(b), w.dy[i].lo10);
    u8g2.setFont(F_S);
    textC(cx, 62, b);
    if (i) for (int y = 14; y < 64; y += 3) u8g2.drawPixel(i * cw, y);
  }
}
const CardImpl CI_FORECAST = {fc_draw, nullptr, fc_has, slow, nullptr, nullptr, nullptr, false};

// ================================================================== Air
static const char* aqiText(int a) {
  if (a <= 50) return "Good";
  if (a <= 100) return "Moderate";
  if (a <= 150) return "Unhealthy for some";
  if (a <= 200) return "Unhealthy";
  if (a <= 300) return "Very unhealthy";
  return "Hazardous";
}
static bool aq_has(CardCtx& c) { return c.f.aq.ok; }
static void aq_draw(CardCtx& c) {
  header(cardTitle(c.idx));
  const Air& a = c.f.aq;
  if (!a.ok) { emptyState("No air data yet"); return; }
  char b[8];
  snprintf(b, sizeof(b), "%d", a.aqi);
  u8g2.setFont(F_N32);
  u8g2.drawStr(0, 46, b);
  int x = textW(b) + 4;
  u8g2.setFont(F_XS);
  u8g2.drawStr(x, 20, "US AQI");
  u8g2.setFont(F_MB);
  textWrap(x, 32, 128 - x, 10, aqiText(a.aqi), 2);
  // scale 0..300 with a marker
  const int y = 54;
  for (int i = 0; i < 6; i++) u8g2.drawFrame(i * 21 + 1, y, 20, 4);
  int pos = constrain(a.aqi, 0, 300) * 126 / 300 + 1;
  u8g2.drawTriangle(pos - 3, y - 4, pos + 3, y - 4, pos, y - 1);
  u8g2.drawBox(1, y + 1, constrain(pos - 1, 0, 125), 2);
  u8g2.setFont(F_XS);
  char s[40];
  snprintf(s, sizeof(s), "PM2.5 %d.%d   PM10 %d", a.pm25x10 / 10, abs(a.pm25x10 % 10), a.pm10x10 / 10);
  textC(64, 64, s);
}
const CardImpl CI_AIR = {aq_draw, nullptr, aq_has, slow, nullptr, nullptr, nullptr, false};

// ================================================================== Markets
static int mkPick(CardCtx& c, int* idx, int max) {
  char k = c.cfg.arg[0];
  int n = 0;
  for (int i = 0; i < c.f.nmk && n < max; i++)
    if (!k || c.f.mk[i].k == k) idx[n++] = i;
  return n;
}
static bool mk_has(CardCtx& c) { int idx[16]; return mkPick(c, idx, 16) > 0; }
static int mk_pages(CardCtx& c) { int idx[16]; return (mkPick(c, idx, 16) + 1) / 2; }
static void mkRow(const Ticker& t, int y) {
  u8g2.setFont(F_MB);
  u8g2.drawStr(0, y + 9, t.s);
  u8g2.setFont(F_L);
  textR(127, y + 10, t.p);
  if (t.nsp >= 2) spark(0, y + 13, 70, 10, t.sp, t.nsp, 15);
  if (t.k != 'f') {
    char b[12];
    snprintf(b, sizeof(b), "%s%d.%d%%", t.c10 >= 0 ? "+" : "-", abs(t.c10) / 10, abs(t.c10) % 10);
    u8g2.setFont(F_MB);
    int w = textW(b);
    textR(127, y + 23, b);
    int ax = 127 - w - 8, ay = y + 18;
    if (t.c10 >= 0) u8g2.drawTriangle(ax, ay + 3, ax + 6, ay + 3, ax + 3, ay - 2);
    else u8g2.drawTriangle(ax, ay - 2, ax + 6, ay - 2, ax + 3, ay + 3);
  }
}
static void mk_draw(CardCtx& c) {
  int idx[16];
  int n = mkPick(c, idx, 16);
  int pages = (n + 1) / 2;
  int p = autoPage(c, pages, 5000);
  header(cardTitle(c.idx), pages, p);
  if (!n) { emptyState("No prices yet", "Pick coins and stocks in the dashboard."); return; }
  mkRow(c.f.mk[idx[p * 2]], 12);
  if (p * 2 + 1 < n) { dotLine(0, 38, 128); mkRow(c.f.mk[idx[p * 2 + 1]], 39); }
}
static bool mk_gesture(CardCtx& c, uint8_t g) {
  if (g != G_DOUBLE) return false;
  int p = mk_pages(c);
  if (p) c.rt.page = (c.rt.page + 1) % p;
  c.rt.pageAt = millis();
  return true;
}
const CardImpl CI_MARKETS = {mk_draw, mk_gesture, mk_has, slow, nullptr, mk_pages, nullptr, false};

// ================================================================== News
static bool nw_has(CardCtx& c) { return c.f.nnw > 0; }
static int nw_pages(CardCtx& c) { return c.f.nnw; }
static void nw_draw(CardCtx& c) {
  int p = autoPage(c, c.f.nnw, 8000);
  char t[24];
  if (c.f.nnw) snprintf(t, sizeof(t), "%s", c.f.nw[p].s[0] ? c.f.nw[p].s : cardTitle(c.idx));
  else snprintf(t, sizeof(t), "%s", cardTitle(c.idx));
  header(t, c.f.nnw, p);
  if (!c.f.nnw) { emptyState("No headlines yet"); return; }
  u8g2.setFont(F_M);
  textWrap(0, 24, 128, 12, c.f.nw[p].h, 4);
}
static bool nw_gesture(CardCtx& c, uint8_t g) {
  if (g != G_DOUBLE || !c.f.nnw) return false;
  c.rt.page = (c.rt.page + 1) % c.f.nnw;
  c.rt.pageAt = millis();
  return true;
}
const CardImpl CI_NEWS = {nw_draw, nw_gesture, nw_has, slow, nullptr, nw_pages, nullptr, false};

// ================================================================== Quote
static bool qt_has(CardCtx& c) { return c.f.hasQt; }
static void qt_draw(CardCtx& c) {
  if (!c.f.hasQt) { header(cardTitle(c.idx)); emptyState("No quote yet"); return; }
  // big opening quote mark as the only decoration
  u8g2.setFont(F_XL);
  u8g2.drawStr(0, 14, "\"");
  u8g2.setFont(F_M);
  int lines = textWrap(10, 11, 118, 11, c.f.qt.q, 4);
  if (lines >= 4) { /* fall back to a smaller font for long quotes */
    u8g2.setDrawColor(0); u8g2.drawBox(0, 0, 128, 64); u8g2.setDrawColor(1);
    u8g2.setFont(F_XL); u8g2.drawStr(0, 14, "\"");
    u8g2.setFont(F_S);
    textWrap(10, 9, 118, 9, c.f.qt.q, 5);
  }
  u8g2.setFont(F_MB);
  char a[30];
  snprintf(a, sizeof(a), "- %s", c.f.qt.a);
  textR(127, 62, a);
}
const CardImpl CI_QUOTE = {qt_draw, nullptr, qt_has, slow, nullptr, nullptr, nullptr, false};

// ================================================================== Agenda
static bool ag_has(CardCtx& c) { return c.f.nev > 0; }
static void ag_draw(CardCtx& c) {
  header(cardTitle(c.idx));
  if (!c.f.nev) { emptyState("Nothing coming up", S.ical[0] ? nullptr : "Add a calendar link in the dashboard."); return; }
  time_t now = time(nullptr);
  int y = 23;
  for (int i = 0; i < c.f.nev && y <= 63; i++) {
    const CalEv& e = c.f.ev[i];
    char when[16];
    bool nowOn = e.t <= (uint32_t)now && e.e > (uint32_t)now;
    if (nowOn && !e.ad) snprintf(when, sizeof(when), "Now");
    else if (e.ad) {
      time_t tt = e.t;
      struct tm lt, nt;
      localtime_r(&tt, &lt);
      localtime_r(&now, &nt);
      snprintf(when, sizeof(when), lt.tm_yday == nt.tm_yday ? "Today" : "%s", WDAY3[lt.tm_wday]);
    } else fmtClock(when, sizeof(when), e.t, true);
    u8g2.setFont(F_MB);
    u8g2.drawStr(0, y, when);
    int w = max(34, textW(when) + 4);
    u8g2.setFont(F_M);
    textFit(w, y, 128 - w, e.d);
    if (nowOn) u8g2.drawBox(0, y + 1, textW(when), 1);
    y += 13;
  }
}
const CardImpl CI_AGENDA = {ag_draw, nullptr, ag_has, slow, nullptr, nullptr, nullptr, false};

// ================================================================== PC
static bool pc_has(CardCtx&) { return gPc.at && millis() - gPc.at < 30000; }
static void pcBar(int y, const char* label, float pct, float temp) {
  u8g2.setFont(F_S);
  u8g2.drawStr(0, y + 6, label);
  bar(22, y, 70, 7, pct / 100.0f);
  char b[12];
  if (temp > 0) snprintf(b, sizeof(b), "%d%% %dC", (int)pct, (int)temp);
  else snprintf(b, sizeof(b), "%d%%", (int)pct);
  textR(127, y + 6, b);
}
static void pc_draw(CardCtx& c) {
  header(cardTitle(c.idx));
  if (!pc_has(c)) { emptyState("PC not connected", "Run tools/pc_companion.py on your computer."); return; }
  pcBar(14, "CPU", gPc.cpu, gPc.cpuT);
  pcBar(24, "RAM", gPc.ram, 0);
  if (gPc.gpu > 0 || gPc.gpuT > 0) pcBar(34, "GPU", gPc.gpu, gPc.gpuT);
  if (gPc.song[0]) {
    u8g2.setFont(F_MB);
    if (gPc.playing) u8g2.drawTriangle(0, 47, 0, 55, 5, 51);
    else { u8g2.drawBox(0, 47, 2, 8); u8g2.drawBox(3, 47, 2, 8); }
    marquee(9, 55, 119, gPc.song, c.t0);
    u8g2.setFont(F_S);
    textFit(9, 64, 119, gPc.artist);
  }
}
static uint16_t pc_frame(CardCtx&) { return gPc.song[0] ? 50 : 1000; }
const CardImpl CI_PC = {pc_draw, nullptr, pc_has, pc_frame, nullptr, nullptr, nullptr, false};

// ================================================================== Messages
static bool msg_has(CardCtx&) { return gMsgN > 0 && millis() - gMsgs[0].at < 24UL * 3600000; }
static int msg_pages(CardCtx&) { return gMsgN; }
static void msg_draw(CardCtx& c) {
  int p = autoPage(c, gMsgN, 8000);
  if (!gMsgN) { header(cardTitle(c.idx)); emptyState("No messages", "Send one from Telegram, the hub or the dashboard."); return; }
  const Message& m = gMsgs[p];
  char ago[8];
  fmtAgo(ago, sizeof(ago), m.at);
  header(m.from, gMsgN, p);
  u8g2.setFont(F_XS);
  textR(127, 17, ago);
  u8g2.setFont(F_M);
  textWrap(0, 24, 124, 12, m.text, 4);
}
static bool msg_gesture(CardCtx& c, uint8_t g) {
  if (g != G_DOUBLE || !gMsgN) return false;
  c.rt.page = (c.rt.page + 1) % gMsgN;
  c.rt.pageAt = millis();
  return true;
}
const CardImpl CI_MESSAGES = {msg_draw, msg_gesture, msg_has, slow, nullptr, msg_pages, nullptr, false};

// ================================================================== System
static int sys_pages(CardCtx&) { return 2; }
static void sys_draw(CardCtx& c) {
  int p = autoPage(c, 2, 8000);
  header(cardTitle(c.idx), 2, p);
  String ip = wifiIp();
  if (p == 1) {
    char url[40];
    snprintf(url, sizeof(url), "http://%s/", ip.length() ? ip.c_str() : "192.168.4.1");
    qrCode(3, 13, url, 2);
    u8g2.setFont(F_MB);
    u8g2.drawStr(58, 26, "Scan to");
    u8g2.drawStr(58, 37, "open me");
    u8g2.setFont(F_S);
    char h[40];
    snprintf(h, sizeof(h), "%s.local", S.host);
    textFit(58, 51, 70, h);
    textFit(58, 61, 70, ip.c_str());
    return;
  }
  u8g2.setFont(F_S);
  char b[48];
  int y = 20;
  if (wifiOnline()) snprintf(b, sizeof(b), "Wi-Fi %s %ddBm", wifiSsid(), wifiRssi());
  else if (wifiApActive()) snprintf(b, sizeof(b), "Setup hotspot on");
  else snprintf(b, sizeof(b), "Wi-Fi: connecting (%s)", wifiLastReason()[0] ? wifiLastReason() : "...");
  textFit(0, y, 128, b); y += 9;
  snprintf(b, sizeof(b), "IP %s", ip.length() ? ip.c_str() : "-");
  textFit(0, y, 128, b); y += 9;
  const char* hs = !S.hubUrl[0] ? "not set (weather only)" : hubState() == HUB_OK ? "connected" : hubState() == HUB_ERROR ? hubError() : "connecting";
  snprintf(b, sizeof(b), "Hub %s", hs);
  textFit(0, y, 128, b); y += 9;
  uint32_t up = uptimeS();
  snprintf(b, sizeof(b), "Up %lud %luh %lum", (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24), (unsigned long)(up / 60 % 60));
  textFit(0, y, 128, b); y += 9;
  snprintf(b, sizeof(b), "RAM %uK free  v%s%s", (unsigned)(ESP.getFreeHeap() / 1024), FW_VERSION, gSafeMode ? " SAFE" : "");
  textFit(0, y, 128, b);
}
const CardImpl CI_SYSTEM = {sys_draw, nullptr, nullptr, slow, nullptr, sys_pages, nullptr, false};
