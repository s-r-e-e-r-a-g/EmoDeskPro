// Full-screen alerts: goals, wickets, results, messages, alarms, timers.
// Queued (up to 6), shown one at a time, any touch dismisses (alarms: tap =
// snooze, hold = stop). Emo reacts afterwards: excited for our goal, sad for
// a wicket against us.
#include "alerts.h"
#include "gfx.h"
#include "sys.h"
#include "settings.h"
#include "emo.h"
#include "audio.h"
#include "input.h"

struct Alert {
  uint8_t kind;
  int8_t mood;
  int8_t alarmIdx;
  uint16_t secs;
  char title[36];
  char text[200];
};

static Alert q[6];
static uint8_t qn = 0;
static bool showing = false;
static Alert cur;
static uint32_t shownAt = 0;

void alarmSnooze(int idx);   // cards_time.cpp
void alarmStop(int idx);

uint8_t alertKindFromCode(uint16_t c) {
  char a = c & 0xFF, b = c >> 8;
  if (a == 'g') return AK_GOAL;
  if (a == 'v') return AK_VAR;
  if (a == 'k') return AK_KICK;
  if (a == 'f') return AK_FT;
  if (a == 'w') return AK_WICKET;
  if (a == 's') return AK_START;
  if (a == 'r') return AK_RESULT;
  if (a == 'm') return AK_MSG;
  (void)b;
  return AK_NOTIFY;
}

static const char* label(uint8_t k) {
  switch (k) {
    case AK_MSG: return "Message";
    case AK_GOAL: return "Goal";
    case AK_VAR: return "VAR";
    case AK_KICK: return "Kick-off";
    case AK_FT: return "Full time";
    case AK_WICKET: return "Wicket";
    case AK_START: return "Match on";
    case AK_RESULT: return "Result";
    case AK_ALARM: return "Alarm";
    case AK_TIMER: return "Time's up";
    default: return "Notice";
  }
}

// "WICKET!" == "Wicket", but "GOAL! ARS 1-0 CHE" != "Goal"
static bool sameWords(const char* a, const char* b) {
  while (*a || *b) {
    while (*a && !isalnum((unsigned char)*a)) a++;
    while (*b && !isalnum((unsigned char)*b)) b++;
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    if (*a) a++;
    if (*b) b++;
  }
  return true;
}

static bool isSport(uint8_t k) { return k >= AK_GOAL && k <= AK_RESULT; }

void alertPush(uint8_t kind, const char* title, const char* text, int8_t mood, uint16_t secs, int8_t alarmIdx) {
  Alert a;
  memset(&a, 0, sizeof(a));
  a.kind = kind;
  a.mood = mood;
  a.alarmIdx = alarmIdx;
  a.secs = secs ? secs : (kind == AK_ALARM || kind == AK_TIMER ? 0 : isSport(kind) ? 14 : kind == AK_MSG ? 20 : 10);
  safeCopy(a.title, title, sizeof(a.title));
  safeCopy(a.text, text, sizeof(a.text));
  // alarms jump the queue
  if (kind == AK_ALARM || kind == AK_TIMER) {
    if (qn == 6) qn--;
    memmove(q + 1, q, sizeof(Alert) * qn);
    q[0] = a;
    qn++;
    if (showing && cur.kind != AK_ALARM && cur.kind != AK_TIMER) { q[qn < 6 ? qn++ : 5] = cur; showing = false; }
  } else {
    if (qn == 6) { memmove(q, q + 1, sizeof(Alert) * 5); qn--; }
    q[qn++] = a;
  }
  logf("[ALERT] %s: %s %s", label(kind), title ? title : "", text ? text : "");
}

static void show() {
  cur = q[0];
  memmove(q, q + 1, sizeof(Alert) * (qn - 1));
  qn--;
  showing = true;
  shownAt = millis();
  gfxPower(true);
  const char* snd = "alert";
  uint8_t ev = SE_MSG;
  switch (cur.kind) {
    case AK_GOAL: snd = cur.mood < 0 ? "sad" : "goal"; ev = SE_SPORTS; break;
    case AK_WICKET: snd = cur.mood < 0 ? "sad" : "wicket"; ev = SE_SPORTS; break;
    case AK_VAR: case AK_KICK: case AK_START: snd = "alert"; ev = SE_SPORTS; break;
    case AK_FT: case AK_RESULT: snd = cur.mood > 0 ? "happy" : cur.mood < 0 ? "sad" : "alert"; ev = SE_SPORTS; break;
    case AK_MSG: snd = "msg"; ev = SE_MSG; break;
    case AK_ALARM: snd = "alarm"; ev = SE_ALARM; break;
    case AK_TIMER: snd = "timer"; ev = SE_TIMER; break;
  }
  audioPlay(snd, ev, cur.kind == AK_ALARM);
}

static void finish() {
  showing = false;
  if (cur.kind == AK_ALARM) audioStop();
  if (S.reactSports || !isSport(cur.kind)) {
    if (cur.mood > 0) emoSet(isSport(cur.kind) ? M_EXCITED : M_HAPPY, 5000);
    else if (cur.mood < 0) emoSet(M_SAD, 5000);
    else if (cur.kind == AK_MSG) emoSet(M_HAPPY, 3000);
  }
}

bool alertActive() { return showing || qn > 0; }

void alertLoop() {
  if (!showing && qn) show();
  if (showing && cur.secs && millis() - shownAt > cur.secs * 1000UL) finish();
}

bool alertGesture(uint8_t g) {
  if (!showing) return false;
  if (g == G_DOWN || g == G_UP) return true;           // swallow raw edges
  if (millis() - shownAt < 400) return true;            // ignore the touch that was already happening
  if (cur.kind == AK_ALARM && cur.alarmIdx >= 0) {
    if (g == G_HOLD || g == G_LONG) alarmStop(cur.alarmIdx);
    else alarmSnooze(cur.alarmIdx);
  }
  finish();
  return true;
}

uint32_t alertFrameMs() {
  if (!showing) return 1000;
  uint32_t t = millis() - shownAt;
  return t < 1600 ? 60 : 250;
}

static void kindIcon(int x, int y) {
  switch (cur.kind) {
    case AK_GOAL: case AK_VAR: case AK_KICK: case AK_FT: iconBall(x + 5, y + 5, 5, 0); break;
    case AK_WICKET: case AK_START: case AK_RESULT:
      for (int i = 0; i < 3; i++) u8g2.drawBox(x + 1 + i * 4, y + 2, 2, 9);  // stumps
      u8g2.drawHLine(x, y + 1, 12);
      break;
    case AK_MSG: iconMail(x, y + 1); break;
    default: iconBell(x, y); break;
  }
}

void alertDraw() {
  uint32_t t = millis() - shownAt;
  // band with the kind and the clock
  u8g2.drawBox(0, 0, 128, 13);
  u8g2.setDrawColor(0);
  u8g2.setFont(F_MB);
  u8g2.drawStr(18, 10, cur.kind == AK_MSG && cur.title[0] ? cur.title : label(cur.kind));
  kindIcon(2, 1);
  u8g2.setDrawColor(0);
  struct tm tm;
  if (localNow(tm)) {
    char b[10];
    fmtTime(b, sizeof(b), tm.tm_hour, tm.tm_min, S.h24, false);
    u8g2.setFont(F_S);
    textR(125, 9, b);
  }
  u8g2.setDrawColor(1);

  if (cur.kind == AK_ALARM || cur.kind == AK_TIMER) {
    u8g2.setFont(F_XL);
    textC(64, 38, cur.title[0] ? cur.title : label(cur.kind));
    u8g2.setFont(F_M);
    textC(64, 50, cur.text);
    u8g2.setFont(F_XS);
    textC(64, 62, cur.kind == AK_ALARM ? "tap: snooze 9 min   hold: stop" : "tap to dismiss");
    bool flash = (t / 500) % 2;
    if (flash) { u8g2.setDrawColor(2); u8g2.drawBox(0, 14, 128, 50); u8g2.setDrawColor(1); }
    return;
  }
  if (cur.kind == AK_MSG) {
    u8g2.setFont(F_M);
    textWrap(2, 26, 124, 11, cur.text, 4);
  } else {
    // A title that only repeats the band ("Wicket", "Result") is dropped and
    // the text becomes the headline.
    const char* lb = label(cur.kind);
    bool dup = !cur.title[0] || sameWords(cur.title, lb);
    const char* head = dup ? cur.text : cur.title;
    u8g2.setFont(F_L);
    if (textW(head) > 124) u8g2.setFont(F_MB);
    if (dup) textWrap(2, 31, 124, 14, head, 2);
    else {
      textFit(2, 31, 124, head);
      u8g2.setFont(F_M);
      textWrap(2, 45, 124, 11, cur.text, 2);
    }
  }
  // countdown hairline at the bottom
  if (cur.secs) {
    int w = 128 - (int)(t * 128 / (cur.secs * 1000UL));
    if (w > 0) u8g2.drawHLine(0, 63, w);
  }
  // goals and wickets flash the screen three times
  if (isSport(cur.kind) && (cur.kind == AK_GOAL || cur.kind == AK_WICKET) && t < 1500 && (t / 250) % 2 == 0) {
    u8g2.setDrawColor(2);
    u8g2.drawBox(0, 0, 128, 64);
    u8g2.setDrawColor(1);
  }
}
