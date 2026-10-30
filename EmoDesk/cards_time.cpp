// Cards that need no internet: Emo, clocks, focus timer, countdown,
// stopwatch, alarms and the dino game. Plus the alarm engine.
#include "cards.h"
#include "gfx.h"
#include "sys.h"
#include "emo.h"
#include "input.h"
#include "audio.h"
#include "alerts.h"
#include "wifi_mgr.h"
#include <time.h>
#include <math.h>

static void mmss(char* b, size_t n, uint32_t ms, bool tenths = false) {
  uint32_t s = ms / 1000;
  if (s >= 3600) snprintf(b, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  else if (tenths) snprintf(b, n, "%lu:%02lu.%lu", (unsigned long)(s / 60), (unsigned long)(s % 60), (unsigned long)(ms / 100 % 10));
  else snprintf(b, n, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static uint16_t fast(CardCtx&) { return 33; }
static uint16_t secondly(CardCtx&) { return 250; }

// ================================================================== Emo
static bool petHeld = false;
static void emo_draw(CardCtx&) { emoDraw(64, 32, 1.0f); }
static void emo_tick(CardCtx& c) {
  bool visible = cardsCurrent() == c.idx;
  bool h = visible && inputHeld() && inputHeldMs() > 350 && !alertActive();
  if (h != petHeld) { petHeld = h; emoPet(h); }
}
static bool emo_gesture(CardCtx&, uint8_t g) {
  if (g == G_DOUBLE) { emoPoke(); audioPlay("poke", SE_EMO); return true; }
  if (g == G_HOLD) return true;   // petting, handled in tick
  if (g == G_TRIPLE) { emoSet(M_HAPPY, 2000); return false; }
  return false;
}
const CardImpl CI_EMO = {emo_draw, emo_gesture, nullptr, fast, emo_tick, nullptr, nullptr, false};

// ================================================================== Clock
static void clock_draw(CardCtx& c) {
  struct tm t;
  if (!localNow(t)) {
    u8g2.setFont(F_N32);
    textC(64, 40, "--:--");
    u8g2.setFont(F_M);
    textC(64, 58, wifiOnline() ? "Getting the time..." : "Waiting for Wi-Fi");
    return;
  }
  int h = t.tm_hour;
  if (!S.h24) { h %= 12; if (!h) h = 12; }
  char b[8];
  snprintf(b, sizeof(b), S.h24 ? "%02d:%02d" : "%d:%02d", h, t.tm_min);
  u8g2.setFont(F_N42);
  int w = textW(b);
  int x = 64 - w / 2;
  u8g2.drawStr(x, 46, b);
  if (!S.h24) { u8g2.setFont(F_S); u8g2.drawStr(min(x + w + 2, 116), 12, t.tm_hour < 12 ? "am" : "pm"); }
  // seconds as a hairline sweeping under the digits
  int sw = (t.tm_sec * 128) / 60;
  u8g2.drawHLine(0, 50, sw);
  char d[32];
  static const char* const DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  snprintf(d, sizeof(d), "%s %d %s", DAYS[t.tm_wday], t.tm_mday, MON3[t.tm_mon]);
  u8g2.setFont(F_MB);
  textC(64, 62, d);
  // corner extras: temperature and an alarm bell when one is set within 12 h
  if (c.f.wx.ok) {
    char tb[8];
    fmtTemp(tb, sizeof(tb), c.f.wx.t10);
    u8g2.setFont(F_S);
    u8g2.drawStr(0, 7, tb);
    degree(textW(tb) + 1, 0);
  }
}
const CardImpl CI_CLOCK = {clock_draw, nullptr, nullptr, secondly, nullptr, nullptr, nullptr, false};

// ================================================================== Analog
static void analog_draw(CardCtx&) {
  struct tm t;
  bool ok = localNow(t);
  const int cx = 32, cy = 32, r = 30;
  u8g2.drawCircle(cx, cy, r);
  for (int i = 0; i < 12; i++) {
    float a = i * M_PI / 6;
    int len = i % 3 == 0 ? 5 : 2;
    u8g2.drawLine(cx + sinf(a) * (r - 1), cy - cosf(a) * (r - 1), cx + sinf(a) * (r - 1 - len), cy - cosf(a) * (r - 1 - len));
  }
  if (ok) {
    float sa = t.tm_sec * M_PI / 30;
    float ma = (t.tm_min + t.tm_sec / 60.0f) * M_PI / 30;
    float ha = ((t.tm_hour % 12) + t.tm_min / 60.0f) * M_PI / 6;
    // thick hour hand, thin minute, dotted-tip second
    for (int o = -1; o <= 1; o++) u8g2.drawLine(cx + o, cy, cx + o + sinf(ha) * 15, cy - cosf(ha) * 15);
    u8g2.drawLine(cx, cy, cx + sinf(ma) * 24, cy - cosf(ma) * 24);
    u8g2.drawDisc(cx + sinf(sa) * 25, cy - cosf(sa) * 25, 1);
    u8g2.drawDisc(cx, cy, 2);
    static const char* const DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    u8g2.setFont(F_MB);
    textC(98, 14, DAYS[t.tm_wday]);
    char d[4];
    snprintf(d, sizeof(d), "%d", t.tm_mday);
    u8g2.setFont(F_N32);
    textC(98, 50, d);
    u8g2.setFont(F_MB);
    textC(98, 62, MON3[t.tm_mon]);
  }
}
const CardImpl CI_ANALOG = {analog_draw, nullptr, nullptr, secondly, nullptr, nullptr, nullptr, false};

// ================================================================== Focus (pomodoro)
// rt.a = phase end (millis) or remaining ms when paused, rt.b = phase (0 focus, 1 break), rt.c = rounds done
static bool gFocus = false;
bool focusActive() { return gFocus; }

static void pomoMinutes(CardCtx& c, int& focusM, int& breakM) {
  focusM = 25; breakM = 5;
  int f, b;
  if (sscanf(c.cfg.arg, "%d/%d", &f, &b) == 2 && f > 0 && b > 0 && f < 180 && b < 60) { focusM = f; breakM = b; }
}
static uint32_t pomoLeft(CardCtx& c) {
  if (!c.rt.a) return 0;
  if (!c.rt.running) return c.rt.a;
  int32_t left = (int32_t)(c.rt.a - millis());
  return left > 0 ? left : 0;
}
static void pomo_tick(CardCtx& c) {
  bool f = c.rt.running && c.rt.b == 0;
  if (c.rt.running && (int32_t)(millis() - c.rt.a) >= 0) {
    int fm, bm;
    pomoMinutes(c, fm, bm);
    if (c.rt.b == 0) {
      c.rt.c++;
      c.rt.b = 1;
      c.rt.a = millis() + bm * 60000UL;
      alertPush(AK_TIMER, "Break time", "Stand up, stretch, drink water.", 1);
    } else {
      c.rt.b = 0;
      c.rt.a = millis() + fm * 60000UL;
      alertPush(AK_TIMER, "Focus", "Back to it.", 0);
    }
  }
  // only one focus card drives the global flag
  static int owner = -1;
  if (f) { owner = c.idx; gFocus = true; emoBase(M_FOCUS); }
  else if (owner == c.idx) { owner = -1; gFocus = false; emoBase(M_NEUTRAL); }
}
static bool pomo_gesture(CardCtx& c, uint8_t g) {
  int fm, bm;
  pomoMinutes(c, fm, bm);
  if (g == G_DOUBLE) {
    if (!c.rt.a) { c.rt.a = millis() + fm * 60000UL; c.rt.b = 0; c.rt.running = true; }
    else if (c.rt.running) { c.rt.a = pomoLeft(c); c.rt.running = false; }
    else { c.rt.a = millis() + c.rt.a; c.rt.running = true; }
    audioPlay("tap", SE_UI);
    return true;
  }
  if (g == G_HOLD) { c.rt.a = 0; c.rt.b = 0; c.rt.c = 0; c.rt.running = false; return true; }
  return false;
}
static void pomo_draw(CardCtx& c) {
  int fm, bm;
  pomoMinutes(c, fm, bm);
  header(c.cfg.name[0] ? c.cfg.name : "Focus");
  uint32_t total = (c.rt.b ? bm : fm) * 60000UL;
  uint32_t left = c.rt.a ? pomoLeft(c) : total;
  char b[12];
  mmss(b, sizeof(b), left + 999);
  u8g2.setFont(F_N32);
  textC(64, 47, b);
  bar(8, 52, 112, 4, total ? 1.0f - (float)left / total : 0);
  u8g2.setFont(F_S);
  char s[32];
  if (!c.rt.a) snprintf(s, sizeof(s), "double-tap: start %d min", fm);
  else if (!c.rt.running) snprintf(s, sizeof(s), "paused - double-tap");
  else snprintf(s, sizeof(s), c.rt.b ? "break  -  round %lu done" : "focus  -  round %lu", (unsigned long)(c.rt.c + (c.rt.b ? 0 : 1)));
  textC(64, 63, s);
}
static bool pomo_live(CardCtx& c) { return c.rt.running; }
const CardImpl CI_POMODORO = {pomo_draw, pomo_gesture, nullptr, secondly, pomo_tick, nullptr, pomo_live, false};

// ================================================================== Timer
// rt.a = end millis (running) or remaining ms (paused), rt.b = preset index
static int timerPresets(CardCtx& c, int* out, int max) {
  const char* s = c.cfg.arg[0] ? c.cfg.arg : "1,3,5,10,15,30";
  int n = 0;
  while (*s && n < max) {
    int v = atoi(s);
    if (v > 0 && v < 600) out[n++] = v;
    while (*s && *s != ',') s++;
    if (*s) s++;
  }
  if (!n) out[n++] = 5;
  return n;
}
static uint32_t timerLeft(CardCtx& c) {
  if (!c.rt.a) return 0;
  if (!c.rt.running) return c.rt.a;
  int32_t l = (int32_t)(c.rt.a - millis());
  return l > 0 ? l : 0;
}
static void timer_tick(CardCtx& c) {
  if (c.rt.running && (int32_t)(millis() - c.rt.a) >= 0) {
    c.rt.running = false;
    c.rt.a = 0;
    alertPush(AK_TIMER, "Time's up", cardTitle(c.idx), 0);
  }
}
static bool timer_gesture(CardCtx& c, uint8_t g) {
  int p[8];
  int n = timerPresets(c, p, 8);
  if (c.rt.b >= (uint32_t)n) c.rt.b = 0;
  if (g == G_DOUBLE) {
    if (!c.rt.a) { c.rt.a = millis() + p[c.rt.b] * 60000UL; c.rt.running = true; }
    else if (c.rt.running) { c.rt.a = timerLeft(c); c.rt.running = false; }
    else { c.rt.a = millis() + c.rt.a; c.rt.running = true; }
    audioPlay("tap", SE_UI);
    return true;
  }
  if (g == G_HOLD) {
    if (c.rt.a) { c.rt.a = 0; c.rt.running = false; }
    else c.rt.b = (c.rt.b + 1) % n;
    return true;
  }
  return false;
}
static void timer_draw(CardCtx& c) {
  int p[8];
  int n = timerPresets(c, p, 8);
  if (c.rt.b >= (uint32_t)n) c.rt.b = 0;
  header(cardTitle(c.idx));
  uint32_t left = c.rt.a ? timerLeft(c) : p[c.rt.b] * 60000UL;
  char b[12];
  mmss(b, sizeof(b), left + (c.rt.a ? 999 : 0));
  u8g2.setFont(F_N32);
  textC(64, 47, b);
  // preset chips
  u8g2.setFont(F_S);
  int x = 2;
  for (int i = 0; i < n; i++) {
    char s[6];
    snprintf(s, sizeof(s), "%d", p[i]);
    int w = textW(s) + 6;
    if (i == (int)c.rt.b) { u8g2.drawRBox(x, 54, w, 10, 2); u8g2.setDrawColor(0); u8g2.drawStr(x + 3, 62, s); u8g2.setDrawColor(1); }
    else u8g2.drawStr(x + 3, 62, s);
    x += w + 2;
  }
  if (c.rt.a && !c.rt.running && (millis() / 500) % 2) { u8g2.setFont(F_XS); textR(127, 62, "paused"); }
}
static bool timer_live(CardCtx& c) { return c.rt.running; }
const CardImpl CI_TIMER = {timer_draw, timer_gesture, nullptr, secondly, timer_tick, nullptr, timer_live, false};

// ================================================================== Stopwatch
// rt.a = start millis, rt.b = accumulated ms, rt.c = last lap ms
static uint32_t swNow(CardCtx& c) { return c.rt.b + (c.rt.running ? millis() - c.rt.a : 0); }
static bool sw_gesture(CardCtx& c, uint8_t g) {
  if (g == G_DOUBLE) {
    if (c.rt.running) { c.rt.b = swNow(c); c.rt.running = false; }
    else { c.rt.a = millis(); c.rt.running = true; }
    audioPlay("tap", SE_UI);
    return true;
  }
  if (g == G_HOLD) {
    if (c.rt.running) c.rt.c = swNow(c);
    else { c.rt.a = c.rt.b = c.rt.c = 0; }
    return true;
  }
  return false;
}
static void sw_draw(CardCtx& c) {
  header(cardTitle(c.idx));
  char b[16];
  mmss(b, sizeof(b), swNow(c), true);
  u8g2.setFont(F_N32);
  textC(64, 47, b);
  u8g2.setFont(F_S);
  if (c.rt.c) { char l[24], lt[16]; mmss(lt, sizeof(lt), c.rt.c, true); snprintf(l, sizeof(l), "lap %s", lt); textC(64, 62, l); }
  else textC(64, 62, c.rt.running ? "hold for lap" : "double-tap to start");
}
static uint16_t sw_frame(CardCtx& c) { return c.rt.running ? 100 : 500; }
static bool sw_live(CardCtx& c) { return c.rt.running; }
const CardImpl CI_STOPWATCH = {sw_draw, sw_gesture, nullptr, sw_frame, nullptr, nullptr, sw_live, false};

// ================================================================== Alarms
static uint32_t snoozeUntil[MAX_ALARMS];
static int lastFireMin[MAX_ALARMS];
static int lastChimeHour = -1;

void alarmSnooze(int i) { if (i >= 0 && i < MAX_ALARMS) snoozeUntil[i] = millis() + 9 * 60000UL; }
void alarmStop(int i) { if (i >= 0 && i < MAX_ALARMS) snoozeUntil[i] = 0; }

static bool alarmToday(const Alarm& a, int wday) {
  if (!a.days) return true;                   // one-shot
  return a.days & (1 << ((wday + 6) % 7));    // bit0 = Monday
}

void alarmsLoop() {
  static bool init = false;
  if (!init) { init = true; for (int i = 0; i < MAX_ALARMS; i++) lastFireMin[i] = -1; }
  struct tm t;
  if (!localNow(t)) return;
  int m = t.tm_hour * 60 + t.tm_min;
  for (int i = 0; i < S.nAlarms; i++) {
    Alarm& a = S.alarms[i];
    if (snoozeUntil[i] && (int32_t)(millis() - snoozeUntil[i]) >= 0) {
      snoozeUntil[i] = 0;
      char hm[10];
      fmtTime(hm, sizeof(hm), t.tm_hour, t.tm_min, S.h24);
      alertPush(AK_ALARM, hm, a.label[0] ? a.label : "Snoozed alarm", 0, 0, i);
    }
    if (!a.on || a.h * 60 + a.m != m || lastFireMin[i] == m + t.tm_yday * 1440) continue;
    if (!alarmToday(a, t.tm_wday)) continue;
    lastFireMin[i] = m + t.tm_yday * 1440;
    char hm[10];
    fmtTime(hm, sizeof(hm), a.h, a.m, S.h24);
    alertPush(AK_ALARM, hm, a.label[0] ? a.label : "Alarm", 0, 0, i);
    emoWake();
    if (!a.days) { a.on = false; settingsMarkDirty(); }
  }
  if (S.hourly && t.tm_min == 0 && t.tm_hour != lastChimeHour) {
    lastChimeHour = t.tm_hour;
    audioPlay("chime", SE_HOURLY);
  }
}

// next enabled alarm index and minutes until it
static int nextAlarm(int& inMin) {
  struct tm t;
  int best = -1;
  inMin = 0;
  if (!localNow(t)) return -1;
  int now = t.tm_hour * 60 + t.tm_min;
  int bestD = 99999;
  for (int i = 0; i < S.nAlarms; i++) {
    const Alarm& a = S.alarms[i];
    if (!a.on) continue;
    for (int d = 0; d < 8; d++) {
      int wd = (t.tm_wday + d) % 7;
      int delta = d * 1440 + a.h * 60 + a.m - now;
      if (delta <= 0) continue;
      if (!alarmToday(a, wd)) continue;
      if (delta < bestD) { bestD = delta; best = i; }
      break;
    }
  }
  inMin = bestD;
  return best;
}

static bool alarms_gesture(CardCtx& c, uint8_t g) {
  if (!S.nAlarms) return false;
  if (g == G_HOLD) { c.rt.page = (c.rt.page + 1) % S.nAlarms; c.rt.a = 1; return true; }
  if (g == G_DOUBLE) {
    int i = c.rt.a ? c.rt.page : -1;
    int mins;
    if (i < 0) i = nextAlarm(mins);
    if (i < 0) i = 0;
    S.alarms[i].on = !S.alarms[i].on;
    settingsMarkDirty();
    c.rt.page = i;
    c.rt.a = 1;
    return true;
  }
  return false;
}
static void alarms_draw(CardCtx& c) {
  header(cardTitle(c.idx), S.nAlarms, c.rt.a ? c.rt.page : 0);
  if (!S.nAlarms) { emptyState("No alarms", "Add them in the dashboard under Alarms."); return; }
  int mins = 0;
  int i = c.rt.a ? (int)c.rt.page : nextAlarm(mins);
  if (i < 0 || i >= S.nAlarms) i = 0;
  if (c.rt.a) nextAlarm(mins);
  const Alarm& a = S.alarms[i];
  char hm[10];
  fmtTime(hm, sizeof(hm), a.h, a.m, S.h24, false);
  u8g2.setFont(F_N32);
  u8g2.drawStr(2, 46, hm);
  int x = 4 + textW(hm);
  u8g2.setFont(F_S);
  if (!S.h24) u8g2.drawStr(x, 22, a.h < 12 ? "am" : "pm");
  u8g2.setFont(F_MB);
  textFit(x, 34, 126 - x, a.label[0] ? a.label : "Alarm");
  u8g2.setFont(F_S);
  u8g2.drawStr(x, 46, a.on ? "on" : "off");
  // day row
  static const char* const D = "MTWTFSS";
  for (int k = 0; k < 7; k++) {
    char s[2] = {D[k], 0};
    int dx = 4 + k * 10;
    bool onDay = !a.days || (a.days & (1 << k));
    u8g2.drawStr(dx, 62, s);
    if (onDay) u8g2.drawHLine(dx - 1, 63, 7);
  }
  if (a.on && mins > 0 && mins < 99999) {
    char in[16];
    snprintf(in, sizeof(in), "in %dh %02dm", mins / 60, mins % 60);
    textR(127, 62, in);
  }
}
const CardImpl CI_ALARMS = {alarms_draw, alarms_gesture, nullptr, nullptr, nullptr, nullptr, nullptr, false};

// ================================================================== Dino
// rt.a = state (0 ready, 1 play, 2 over), rt.b = score, rt.c = best
struct Dino { float y, vy; float ox[3]; uint8_t oh[3]; float speed; uint32_t last, overAt; };
static Dino dn;

static void dinoReset() {
  dn.y = 0; dn.vy = 0; dn.speed = 70;
  for (int i = 0; i < 3; i++) { dn.ox[i] = 140 + i * 60 + (esp_random() % 30); dn.oh[i] = 6 + esp_random() % 8; }
  dn.last = millis();
}
static bool dino_gesture(CardCtx& c, uint8_t g) {
  if (g != G_DOWN) return g == G_DOUBLE || g == G_TAP;   // taps never leave the game
  if (c.rt.a == 0 || (c.rt.a == 2 && millis() - dn.overAt > 600)) { dinoReset(); c.rt.a = 1; c.rt.b = 0; return true; }
  if (c.rt.a == 1 && dn.y <= 0.01f) { dn.vy = 165; audioPlay("tap", SE_UI); }
  return true;
}
static void dino_draw(CardCtx& c) {
  uint32_t now = millis();
  float dt = (now - dn.last) / 1000.0f;
  if (dt > 0.05f) dt = 0.05f;
  dn.last = now;
  const int ground = 56;
  if (c.rt.a == 1) {
    dn.vy -= 520 * dt;
    dn.y += dn.vy * dt;
    if (dn.y < 0) { dn.y = 0; dn.vy = 0; }
    dn.speed += 2.5f * dt;
    for (int i = 0; i < 3; i++) {
      dn.ox[i] -= dn.speed * dt;
      if (dn.ox[i] < -8) {
        float far = 0;
        for (int k = 0; k < 3; k++) far = max(far, dn.ox[k]);
        dn.ox[i] = max(128.0f, far + 45 + (esp_random() % 50));
        dn.oh[i] = 6 + esp_random() % 9;
        c.rt.b++;
      }
      // collision: dino box x 10..20
      int dy = ground - (int)dn.y;
      if (dn.ox[i] < 20 && dn.ox[i] + 5 > 11 && dy > ground - dn.oh[i] + 1) {
        c.rt.a = 2;
        dn.overAt = now;
        if (c.rt.b > c.rt.c) c.rt.c = c.rt.b;
        audioPlay("sad", SE_UI);
      }
    }
  }
  u8g2.drawHLine(0, ground + 1, 128);
  for (int x = (int)(-(now / 30) % 16); x < 128; x += 16) u8g2.drawPixel(x + 7, ground + 3);
  // dino
  int dy = ground - (int)dn.y;
  u8g2.drawBox(12, dy - 12, 8, 9);
  u8g2.drawBox(17, dy - 15, 7, 5);
  u8g2.setDrawColor(0); u8g2.drawPixel(21, dy - 14); u8g2.setDrawColor(1);
  bool step = (now / 120) % 2 && dn.y <= 0;
  u8g2.drawVLine(step ? 13 : 14, dy - 3, 3);
  u8g2.drawVLine(step ? 18 : 17, dy - 3, 3);
  u8g2.drawPixel(11, dy - 10);
  // cacti
  for (int i = 0; i < 3; i++) {
    int x = (int)dn.ox[i];
    if (x > 130) continue;
    u8g2.drawBox(x + 1, ground - dn.oh[i] + 1, 3, dn.oh[i]);
    u8g2.drawBox(x, ground - dn.oh[i] / 2, 1, 3);
    u8g2.drawBox(x + 4, ground - dn.oh[i] / 2 - 2, 1, 3);
  }
  char s[24];
  u8g2.setFont(F_S);
  snprintf(s, sizeof(s), "%lu", (unsigned long)c.rt.b);
  textR(127, 8, s);
  if (c.rt.c) { snprintf(s, sizeof(s), "best %lu", (unsigned long)c.rt.c); u8g2.drawStr(0, 8, s); }
  if (c.rt.a != 1) {
    u8g2.setFont(F_MB);
    textC(64, 28, c.rt.a == 0 ? "Touch to jump" : "Game over");
    u8g2.setFont(F_XS);
    textC(64, 38, "hold 1.5 s to leave");
  }
}
const CardImpl CI_DINO = {dino_draw, dino_gesture, nullptr, fast, nullptr, nullptr, nullptr, true};
