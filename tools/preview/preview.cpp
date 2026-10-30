// Renders every EmoDesk screen on a PC into PBM images, using the real card,
// gfx, emo and alert code with demo data. Used to review the OLED layouts.
//   tools/preview/run.sh  ->  tools/preview/out/*.png
#include "Arduino.h"
#include "../../EmoDesk/sys.h"
#include "../../EmoDesk/settings.h"
#include "../../EmoDesk/feed.h"
#include "../../EmoDesk/gfx.h"
#include "../../EmoDesk/cards.h"
#include "../../EmoDesk/emo.h"
#include "../../EmoDesk/alerts.h"
#include "../../EmoDesk/wifi_mgr.h"
#include "../../EmoDesk/input.h"
#include "../../EmoDesk/audio.h"
#include <cstdarg>
#include <random>

uint32_t g_millis = 100000;
EspClass ESP;
static std::mt19937 rng(7);
uint32_t esp_random() { return rng(); }

// ---------------------------------------------------------------- sys stubs
bool gSafeMode = false;
const char* const WDAY3[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const MON3[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static time_t fakeNow = 0;
void logf(const char* fmt, ...) { (void)fmt; }
size_t logCopy(char*, size_t) { return 0; }
bool timeValid() { return true; }
bool localNow(struct tm& t) { localtime_r(&fakeNow, &t); return true; }
int minuteOfDay() { struct tm t; localNow(t); return t.tm_hour * 60 + t.tm_min; }
int parseHM(const char* s) { int h, m; return s && sscanf(s, "%d:%d", &h, &m) == 2 ? h * 60 + m : -1; }
bool inWindow(int m, int from, int to) { if (m < 0 || from < 0 || to < 0 || from == to) return false; return from < to ? (m >= from && m < to) : (m >= from || m < to); }
void fmtTime(char* out, size_t n, int h, int m, bool h24, bool suffix) {
  if (h24) { snprintf(out, n, "%02d:%02d", h, m); return; }
  int hh = h % 12; if (!hh) hh = 12;
  if (suffix) snprintf(out, n, "%d:%02d%s", hh, m, h < 12 ? "am" : "pm"); else snprintf(out, n, "%d:%02d", hh, m);
}
void safeCopy(char* d, const char* s, size_t n) { if (!d || !n) return; if (!s) { d[0] = 0; return; } strncpy(d, s, n - 1); d[n - 1] = 0; }
uint32_t uptimeS() { return 93784; }
size_t heapLargest() { return 90000; }
bool saveFileAtomic(const char*, const char*, size_t) { return true; }
String readFile(const char*) { return String(); }
void ledSet(bool) {}
const char* resetReasonStr() { return "power on"; }
bool busPost(uint8_t, const char*, const char*, int16_t, int16_t) { return true; }

// ---------------------------------------------------------------- feed / net / wifi / input / audio stubs
static Feed F;
PcStats gPc;
Message gMsgs[4];
uint8_t gMsgN = 0;
void feedLock() {}
void feedUnlock() {}
const Feed& feed() { return F; }
uint32_t feedVersion() { return 1; }
HubState hubState() { return HUB_OK; }
const char* hubError() { return ""; }
uint32_t hubLastOkS() { return 1; }
void addMessage(const char* from, const char* text) {
  safeCopy(gMsgs[0].from, from, sizeof(gMsgs[0].from));
  safeCopy(gMsgs[0].text, text, sizeof(gMsgs[0].text));
  gMsgs[0].at = g_millis - 240000;
  gMsgN = 1;
}
bool feedHasLiveMine() { return true; }
bool wifiOnline() { return true; }
bool wifiApActive() { return false; }
WifiState wifiState() { return WF_CONNECTED; }
const char* wifiSsid() { return "HomeNet"; }
String wifiIp() { return String("192.168.1.42"); }
int wifiRssi() { return -58; }
const char* wifiLastReason() { return ""; }
static bool held = false;
bool inputHeld() { return held; }
uint32_t inputHeldMs() { return held ? 800 : 0; }
uint32_t inputLastMs() { return g_millis; }
void inputInject(uint8_t) {}
const char* gestureName(uint8_t) { return ""; }
void audioBegin() {}
void audioApply() {}
bool audioPlay(const char*, uint8_t, bool) { return true; }
void audioStop() {}
bool audioBusy() { return false; }

// ---------------------------------------------------------------- demo data
static void demoFeed() {
  memset(&F, 0, sizeof(F));
  time_t now = fakeNow;
  Weather& w = F.wx;
  w.ok = true; w.t10 = 274; w.f10 = 291; w.hum = 61; w.code = 2; w.day = true; w.hi10 = 300; w.lo10 = 210; w.wind10 = 112;
  strcpy(w.sr, "06:12"); strcpy(w.ss, "17:48");
  w.nh = 12;
  for (int i = 0; i < 12; i++) { w.hr[i].h = (20 + i) % 24; w.hr[i].t10 = 270 - i * 6 + (i > 8 ? (i - 8) * 8 : 0); w.hr[i].code = i < 4 ? 2 : 61; w.hr[i].pop = i < 3 ? 10 : 60; }
  w.nd = 5;
  int codes[] = {2, 61, 3, 0, 95};
  for (int i = 0; i < 5; i++) { w.dy[i].wd = (5 + i) % 7; w.dy[i].code = codes[i]; w.dy[i].hi10 = 300 - i * 5; w.dy[i].lo10 = 210 + i * 3; }
  F.aq = {true, 88, 301, 500, 40};

  auto fb = [&](int i, const char* lg, const char* lid, uint8_t st, const char* d, int ofs, const char* ha, const char* hn, int hs, const char* aa, const char* an, int as, bool mine, const char* gh, const char* ga) {
    FbMatch& m = F.fb[i];
    strcpy(m.lg, lg); strcpy(m.lid, lid); m.st = st; strcpy(m.d, d); m.t = now + ofs; m.mine = mine;
    strcpy(m.h.a, ha); strcpy(m.h.n, hn); m.h.s = hs; strcpy(m.a.a, aa); strcpy(m.a.n, an); m.a.s = as;
    strcpy(m.h.goals, gh); strcpy(m.a.goals, ga);
    strcpy(m.h.id, i == 0 ? "359" : "1"); strcpy(m.a.id, "2");
  };
  fb(0, "EPL", "eng.1", MS_IN, "67'", -4000, "ARS", "Arsenal", 2, "CHE", "Chelsea", 1, true, "B. Saka 23', K. Havertz 61'", "C. Palmer 55' (p)");
  fb(1, "EPL", "eng.1", MS_PRE, "", 86400 + 3600, "LIV", "Liverpool", -1, "MCI", "Man City", -1, false, "", "");
  fb(2, "UCL", "uefa.champions", MS_POST, "FT", -86400 * 2, "RMA", "Real Madrid", 3, "ARS", "Arsenal", 3, true, "", "");
  F.fb[2].h.red = 1;
  F.nfb = 3;

  CrMatch& c = F.cr[0];
  strcpy(c.ser, "Australia tour of India"); strcpy(c.ti, "IND v AUS"); strcpy(c.f, "T20"); c.st = MS_IN;
  strcpy(c.status, "India need 44 runs in 22 balls"); c.mine = true; c.crr10 = 89; c.rrr10 = 120;
  strcpy(c.s[0].a, "IND"); strcpy(c.s[0].sc, "145/4"); strcpy(c.s[0].ov, "16.2"); c.s[0].bat = true;
  strcpy(c.s[1].a, "AUS"); strcpy(c.s[1].sc, "188/6"); strcpy(c.s[1].ov, "20");
  CrMatch& c2 = F.cr[1];
  strcpy(c2.ser, "Australia tour of India"); strcpy(c2.ti, "4th T20I, Rajkot"); strcpy(c2.f, "T20"); c2.st = MS_PRE; c2.t = now + 2 * 86400;
  strcpy(c2.s[0].a, "IND"); strcpy(c2.s[1].a, "AUS"); c2.mine = true;
  CrMatch& c3 = F.cr[2];
  strcpy(c3.ser, "Ranji Trophy"); strcpy(c3.ti, "MUM v KAR"); strcpy(c3.f, "TEST"); c3.st = MS_POST;
  strcpy(c3.status, "Mumbai won by an innings and 23 runs"); strcpy(c3.s[0].a, "MUM"); strcpy(c3.s[0].sc, "512/8"); c3.s[0].win = true;
  strcpy(c3.s[1].a, "KAR"); strcpy(c3.s[1].sc, "210 & 279");
  F.ncr = 3;

  Table& t = F.tb[0];
  strcpy(t.lg, "Premier League"); strcpy(t.lid, "eng.1");
  const char* teams[] = {"ARS", "LIV", "MCI", "CHE", "AVL", "NEW", "TOT", "BHA", "MUN", "BOU"};
  for (int i = 0; i < 10; i++) { t.r[i] = {(uint8_t)(i + 1), "", (int16_t)(25 - i * 2), 10, (int16_t)(14 - i * 3), i == 0}; strcpy(t.r[i].a, teams[i]); }
  t.n = 10;
  F.ntb = 1;

  auto tk = [&](int i, const char* s, const char* p, int c10, char k) {
    Ticker& x = F.mk[i];
    strcpy(x.s, s); strcpy(x.p, p); x.c10 = c10; x.k = k;
    x.nsp = k == 'f' ? 0 : 24;
    for (int j = 0; j < x.nsp; j++) x.sp[j] = (uint8_t)(7 + 6 * sin(j * 0.4 + i) * (c10 > 0 ? 1 : -1) + (c10 > 0 ? j / 4 : -j / 4)) % 16;
  };
  tk(0, "BTC", "67,321", 124, 'c');
  tk(1, "ETH", "3,412.55", -238, 'c');
  tk(2, "RELIANCE", "2,931.40", 41, 's');
  tk(3, "USD/INR", "84.07", 0, 'f');
  F.nmk = 4;

  strcpy(F.nw[0].h, "Leaders agree on a new climate fund after two weeks of tense talks in Belem");
  strcpy(F.nw[0].s, "BBC");
  strcpy(F.nw[1].h, "Scientists find a new species of deep-sea octopus");
  strcpy(F.nw[1].s, "BBC");
  F.nnw = 2;
  strcpy(F.qt.q, "It does not matter how slowly you go as long as you do not stop.");
  strcpy(F.qt.a, "Confucius");
  F.hasQt = true;

  F.ev[0] = {(uint32_t)(now + 1800), (uint32_t)(now + 3600), "", false}; strcpy(F.ev[0].d, "Standup with the platform team");
  F.ev[1] = {(uint32_t)(now + 86400), (uint32_t)(now + 90000), "", false}; strcpy(F.ev[1].d, "Dentist");
  F.ev[2] = {(uint32_t)(now + 86400 * 3), (uint32_t)(now + 86400 * 4), "", true}; strcpy(F.ev[2].d, "Diwali");
  F.nev = 3;

  gPc = {g_millis, 37, 62, 18, 61, 54, "Blinding Lights", "The Weeknd", true, 0, false};
  addMessage("Mum", "Dinner is ready! Come down when you finish the match.");
}

// ---------------------------------------------------------------- output
static void savePbm(const char* name) {
  char path[256];
  snprintf(path, sizeof(path), "out/%s.pbm", name);
  FILE* f = fopen(path, "wb");
  fprintf(f, "P4\n128 64\n");
  const uint8_t* b = gfxFrame();
  // page format -> rows
  for (int y = 0; y < 64; y++) {
    for (int xb = 0; xb < 16; xb++) {
      uint8_t v = 0;
      for (int k = 0; k < 8; k++) if (b[(y / 8) * 128 + xb * 8 + k] & (1 << (y % 8))) v |= 0x80 >> k;
      fputc(v, f);
    }
  }
  fclose(f);
}

static void frame(const char* name, void (*draw)()) {
  gfxClear();
  draw();
  gfxPresent();
  savePbm(name);
}

static int curCard = 0;
static void drawCard() { cardsDraw(); }

int main() {
  setenv("TZ", "IST-5:30", 1);
  tzset();
  // Fri 30 Oct 2026, 19:42:15 IST
  struct tm t = {};
  t.tm_year = 126; t.tm_mon = 9; t.tm_mday = 30; t.tm_hour = 19; t.tm_min = 42; t.tm_sec = 15; t.tm_isdst = -1;
  fakeNow = mktime(&t);
  settingsDefaults(S);
  S.burnShift = false;
  S.slide = false;
  S.skipEmpty = false;
  safeCopy(S.place, "Bengaluru", sizeof(S.place));
  safeCopy(S.hubUrl, "http://hub.local:8787", sizeof(S.hubUrl));
  S.nFbTeams = 1; strcpy(S.fbTeams[0].id, "359"); strcpy(S.fbTeams[0].abbr, "ARS");
  S.nAlarms = 2;
  S.alarms[0] = {true, 6, 45, 0x1F, "Gym"};
  S.alarms[1] = {true, 9, 0, 0x60, "Weekend"};
  demoFeed();
  gfxBegin();
  emoBegin();
  for (int i = 0; i < CT_COUNT; i++) { memset(&gCards[i], 0, sizeof(CardCfg)); gCards[i].type = i; gCards[i].on = true; }
  gCardN = CT_COUNT;
  cardsBegin();

  for (int i = 0; i < CT_COUNT; i++) {
    cardsGoto(i);
    curCard = i;
    g_millis += 300;
    emoUpdate();
    char name[40];
    snprintf(name, sizeof(name), "%02d_%s", i, CARD_TYPES[i].key);
    frame(name, drawCard);
    // second page for multi-page cards
    if (i == CT_WEATHER || i == CT_FOOTBALL || i == CT_CRICKET || i == CT_MARKETS || i == CT_SYSTEM || i == CT_TABLE) {
      for (int p = 1; p < 3; p++) {
        g_millis += 9000;
        snprintf(name, sizeof(name), "%02d_%s_p%d", i, CARD_TYPES[i].key, p + 1);
        frame(name, drawCard);
        if (i == CT_WEATHER || i == CT_SYSTEM) break;
      }
    }
  }
  // Emo moods
  cardsGoto(CT_EMO);
  const uint8_t moods[] = {M_NEUTRAL, M_HAPPY, M_SAD, M_ANGRY, M_SURPRISED, M_SLEEPY, M_LOVE, M_EXCITED, M_WINK, M_DIZZY, M_SLEEP, M_FOCUS};
  for (uint8_t m : moods) {
    emoSet(m, 60000);
    for (int k = 0; k < 40; k++) { g_millis += 33; emoUpdate(); }
    char name[40];
    snprintf(name, sizeof(name), "emo_%s", moodName(m));
    frame(name, [] { emoDraw(64, 32, 1.0f); });
  }
  // alerts
  struct A { uint8_t k; const char* ti; const char* tx; const char* name; };
  A al[] = {
    {AK_GOAL, "GOAL! ARS 3-1 CHE", "B. Saka 78'", "alert_goal"},
    {AK_WICKET, "WICKET!", "IND 146/5 (16.4)", "alert_wicket"},
    {AK_MSG, "Mum", "Dinner is ready! Come down when you finish the match.", "alert_message"},
    {AK_ALARM, "6:45am", "Gym", "alert_alarm"},
    {AK_RESULT, "Result", "India won by 6 wickets (with 4 balls remaining)", "alert_result"},
  };
  for (auto& a : al) {
    alertPush(a.k, a.ti, a.tx, 1);
    alertLoop();
    g_millis += 1700;   // past the flash
    frame(a.name, [] { alertDraw(); });
    alertGesture(G_TAP);
    g_millis += 1000;
    alertLoop();
    alertGesture(G_TAP);
  }
  printf("ok\n");
  return 0;
}
