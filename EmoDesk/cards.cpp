#include "cards.h"
#include "gfx.h"
#include "sys.h"
#include "input.h"
#include "emo.h"
#include "wifi_mgr.h"
#include <time.h>

const CardImpl* const CARD_IMPL[CT_COUNT] = {
  &CI_EMO, &CI_CLOCK, &CI_ANALOG, &CI_WEATHER, &CI_FORECAST, &CI_AIR, &CI_CRICKET, &CI_FOOTBALL, &CI_TABLE,
  &CI_MARKETS, &CI_NEWS, &CI_QUOTE, &CI_AGENDA, &CI_POMODORO, &CI_TIMER, &CI_STOPWATCH, &CI_ALARMS, &CI_PC,
  &CI_MESSAGES, &CI_SYSTEM, &CI_DINO,
};

static CardRt rt[MAX_CARDS];
static int cur = 0;
static uint32_t enteredAt = 0;
static bool pinned = false;
static int pendingSlide = 0;

static CardCtx ctx(int i) {
  CardCtx c{i, gCards[i], rt[i], enteredAt, feed()};
  return c;
}

const char* cardTitle(int i) {
  if (i < 0 || i >= gCardN) return "";
  return gCards[i].name[0] ? gCards[i].name : CARD_TYPES[gCards[i].type].title;
}
const char* cardsCurrentTitle() { return cardTitle(cur); }
int cardsCurrent() { return cur; }
bool cardsPinned() { return pinned; }
bool cardsCurrentRaw() { return gCardN && CARD_IMPL[gCards[cur].type]->raw; }

static bool hasData(int i) {
  const CardImpl* im = CARD_IMPL[gCards[i].type];
  if (!im->hasData) return true;
  feedLock();
  CardCtx c = ctx(i);
  bool r = im->hasData(c);
  feedUnlock();
  return r;
}

static void enter(int i, int dir) {
  if (i < 0 || i >= gCardN) return;
  if (i != cur && dir) pendingSlide = dir;
  cur = i;
  enteredAt = millis();
  rt[i].pageAt = enteredAt;
  if (!CARD_IMPL[gCards[i].type]->raw) rt[i].page = rt[i].page;   // keep page across visits
}

void cardsBegin() {
  memset(rt, 0, sizeof(rt));
  cur = 0;
  for (int i = 0; i < gCardN; i++) if (gCards[i].on) { cur = i; break; }
  enteredAt = millis();
}

void cardsChanged() {
  memset(rt, 0, sizeof(rt));
  pinned = false;
  if (cur >= gCardN || !gCards[cur].on) cardsHome();
}

void cardsNext(int dir) {
  if (!gCardN) return;
  for (int pass = 0; pass < 2; pass++) {
    for (int k = 1; k <= gCardN; k++) {
      int j = ((cur + dir * k) % gCardN + gCardN) % gCardN;
      if (!gCards[j].on) continue;
      // first pass skips empty cards; if everything is empty, show anything
      if (pass == 0 && S.skipEmpty && !hasData(j)) continue;
      if (j == cur) return;
      enter(j, dir);
      return;
    }
  }
}

void cardsGoto(int i) {
  if (i >= 0 && i < gCardN) enter(i, i >= cur ? 1 : -1);
}

void cardsHome() {
  for (int i = 0; i < gCardN; i++)
    if (gCards[i].on && gCards[i].type == CT_EMO) { if (i != cur) enter(i, -1); return; }
  for (int i = 0; i < gCardN; i++) if (gCards[i].on) { enter(i, -1); return; }
}

void cardsPinToggle() { pinned = !pinned; }

static uint32_t durationMs(int i) {
  const CardImpl* im = CARD_IMPL[gCards[i].type];
  uint32_t d = (gCards[i].dur ? gCards[i].dur : S.rotateSec) * 1000UL;
  if (im->pages) {
    feedLock();
    CardCtx c = ctx(i);
    int p = im->pages(c);
    bool lv = im->live && im->live(c);
    feedUnlock();
    if (p > 1) d = max<uint32_t>(d, min<uint32_t>(p * 5000UL, 40000));
    if (lv) d *= 2;
  }
  return d;
}

void cardsLoop() {
  if (!gCardN) return;
  uint32_t now = millis();
  feedLock();
  for (int i = 0; i < gCardN; i++) {
    const CardImpl* im = CARD_IMPL[gCards[i].type];
    if (im->tick) { CardCtx c = ctx(i); im->tick(c); }
  }
  feedUnlock();
  if (!gCards[cur].on) { cardsNext(1); return; }
  const CardImpl* im = CARD_IMPL[gCards[cur].type];
  if (im->raw || pinned || focusActive()) return;
  uint32_t idle = now - inputLastMs();
  if (S.rotate) {
    if (idle > 8000 && now - enteredAt > durationMs(cur)) cardsNext(1);
  } else if (S.homeAfterSec && idle > S.homeAfterSec * 1000UL && now - enteredAt > S.homeAfterSec * 1000UL && gCards[cur].type != CT_EMO) {
    cardsHome();
  }
}

void cardsDraw() {
  if (!gCardN) { emptyState("No cards", "Add cards in the dashboard."); return; }
  if (pendingSlide) { gfxBeginSlide(pendingSlide); pendingSlide = 0; }
  const CardImpl* im = CARD_IMPL[gCards[cur].type];
  feedLock();
  CardCtx c = ctx(cur);
  im->draw(c);
  feedUnlock();
  if (pinned && !im->raw) {   // tiny pin marker bottom-right
    u8g2.drawBox(124, 60, 3, 3);
  }
}

uint16_t cardsFrameMs() {
  if (!gCardN) return 1000;
  const CardImpl* im = CARD_IMPL[gCards[cur].type];
  if (!im->frameMs) return 500;
  feedLock();
  CardCtx c = ctx(cur);
  uint16_t ms = im->frameMs(c);
  feedUnlock();
  return ms;
}

bool cardsGesture(uint8_t g) {
  if (!gCardN) return false;
  const CardImpl* im = CARD_IMPL[gCards[cur].type];
  if (!im->gesture) return false;
  feedLock();
  CardCtx c = ctx(cur);
  bool r = im->gesture(c, g);
  feedUnlock();
  if (r) enteredAt = millis() - min<uint32_t>(millis() - enteredAt, 2000);   // interacting: don't rotate away
  return r;
}

// ------------------------------------------------------------------ helpers
int autoPage(CardCtx& c, int n, uint32_t everyMs) {
  if (n <= 1) { c.rt.page = 0; return 0; }
  uint32_t now = millis();
  if (now - c.rt.pageAt >= everyMs) { c.rt.page = (c.rt.page + 1) % n; c.rt.pageAt = now; }
  if (c.rt.page >= n) c.rt.page = 0;
  return c.rt.page;
}

void fmtTemp(char* b, size_t n, int16_t t10) {
  int v = (t10 >= 0 ? t10 + 5 : t10 - 5) / 10;
  snprintf(b, n, "%d", v);
}

void fmtAgo(char* b, size_t n, uint32_t since) {
  uint32_t s = (millis() - since) / 1000;
  if (s < 60) snprintf(b, n, "now");
  else if (s < 3600) snprintf(b, n, "%lum", (unsigned long)(s / 60));
  else if (s < 86400) snprintf(b, n, "%luh", (unsigned long)(s / 3600));
  else snprintf(b, n, "%lud", (unsigned long)(s / 86400));
}

// "19:30", "Sat 19:30", "Today 7:30pm"
void fmtClock(char* b, size_t n, uint32_t t, bool withDay) {
  time_t tt = t;
  struct tm lt, nowT;
  localtime_r(&tt, &lt);
  char hm[10];
  fmtTime(hm, sizeof(hm), lt.tm_hour, lt.tm_min, S.h24);
  if (!withDay || !localNow(nowT)) { snprintf(b, n, "%s", hm); return; }
  int dd = (lt.tm_year - nowT.tm_year) * 366 + lt.tm_yday - nowT.tm_yday;
  if (dd == 0) snprintf(b, n, "%s", hm);
  else if (dd == 1) snprintf(b, n, "Tmrw %s", hm);
  else if (dd == -1) snprintf(b, n, "Yday %s", hm);
  else if (dd > 1 && dd < 7) snprintf(b, n, "%s %s", WDAY3[lt.tm_wday], hm);
  else snprintf(b, n, "%d %s", lt.tm_mday, MON3[lt.tm_mon]);
}

void fmtDay(char* b, size_t n, uint32_t t) {
  time_t tt = t;
  struct tm lt, nowT;
  localtime_r(&tt, &lt);
  if (!localNow(nowT)) { snprintf(b, n, "%d %s", lt.tm_mday, MON3[lt.tm_mon]); return; }
  int dd = (lt.tm_year - nowT.tm_year) * 366 + lt.tm_yday - nowT.tm_yday;
  if (dd == 0) snprintf(b, n, "Today");
  else if (dd == 1) snprintf(b, n, "Tomorrow");
  else if (dd > 1 && dd < 7) snprintf(b, n, "%s", WDAY3[lt.tm_wday]);
  else snprintf(b, n, "%d %s", lt.tm_mday, MON3[lt.tm_mon]);
}
