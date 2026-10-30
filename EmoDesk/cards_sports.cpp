// Sports cards. All matching, ordering and alert detection happens on the
// hub; the device only lays out what it receives, so these cards are simple
// and never stall on network work.
#include "cards.h"
#include "gfx.h"
#include "sys.h"
#include "input.h"
#include <time.h>

static void liveChip(int x, int y, const char* txt, bool blinkDot) {
  u8g2.setFont(F_MB);
  int w = textW(txt) + 10;
  u8g2.drawRBox(x - w / 2, y - 9, w, 11, 3);
  u8g2.setDrawColor(0);
  if (!blinkDot || (millis() / 600) % 2) u8g2.drawDisc(x - w / 2 + 4, y - 4, 1);
  u8g2.drawStr(x - w / 2 + 8, y, txt);
  u8g2.setDrawColor(1);
}

// ================================================================== Football
static int fbPick(CardCtx& c, int* idx, int max) {
  int n = 0;
  for (int i = 0; i < c.f.nfb && n < max; i++) {
    const FbMatch& m = c.f.fb[i];
    if (c.cfg.arg[0] && strcmp(m.lid, c.cfg.arg)) continue;
    idx[n++] = i;
  }
  return n;
}
static bool fb_has(CardCtx& c) { int idx[14]; return fbPick(c, idx, 14) > 0; }
static int fb_pages(CardCtx& c) { int idx[14]; return min(fbPick(c, idx, 14), 8); }
static bool fb_live(CardCtx& c) {
  int idx[14];
  int n = fbPick(c, idx, 14);
  for (int i = 0; i < n; i++) if (c.f.fb[idx[i]].st == MS_IN && c.f.fb[idx[i]].mine) return true;
  return false;
}

static void fbTeam(int cx, const FbSide& s, bool followed, bool winner) {
  u8g2.setFont(F_L);
  textC(cx, 30, s.a);
  int w = textW(s.a);
  if (followed) u8g2.drawHLine(cx - w / 2, 32, w);
  for (int r = 0; r < s.red && r < 3; r++) u8g2.drawBox(cx + w / 2 + 2 + r * 3, 21, 2, 5);
  u8g2.setFont(F_XS);
  char n[11];
  safeCopy(n, s.n, sizeof(n));
  textC(cx, 40, n);
  (void)winner;
}

static void fb_draw(CardCtx& c) {
  int idx[14];
  int n = min(fbPick(c, idx, 14), 8);
  // live matches of followed teams get the screen first and longest
  int p = autoPage(c, n, 6000);
  if (!n) {
    header(cardTitle(c.idx));
    emptyState("No matches", S.hubUrl[0] ? "Nothing live or coming up for your leagues." : "Football needs the hub. Set it up in the dashboard.");
    return;
  }
  const FbMatch& m = c.f.fb[idx[p]];
  char title[24];
  snprintf(title, sizeof(title), "%s", m.lg[0] ? m.lg : cardTitle(c.idx));
  header(title, n, p);
  bool hf = false, af = false;
  for (int i = 0; i < S.nFbTeams; i++) {
    hf |= m.h.id[0] && !strcmp(S.fbTeams[i].id, m.h.id);
    af |= m.a.id[0] && !strcmp(S.fbTeams[i].id, m.a.id);
  }
  fbTeam(18, m.h, hf, false);
  fbTeam(110, m.a, af, false);
  if (m.st == MS_PRE) {
    time_t tt = m.t;
    struct tm lt;
    localtime_r(&tt, &lt);
    char hm[10];
    fmtTime(hm, sizeof(hm), lt.tm_hour, lt.tm_min, S.h24, false);
    u8g2.setFont(F_L);
    textC(64, 32, hm);
    char day[20];
    fmtDay(day, sizeof(day), m.t);
    u8g2.setFont(F_S);
    textC(64, 42, day);
  } else {
    char sc[10];
    snprintf(sc, sizeof(sc), "%d-%d", max<int>(0, m.h.s), max<int>(0, m.a.s));
    u8g2.setFont(strlen(sc) > 3 ? F_N18 : F_N22);
    textC(64, 37, sc);
    if (m.st == MS_IN) liveChip(64, 51, m.d[0] ? m.d : "LIVE", true);
    else { u8g2.setFont(F_MB); textC(64, 51, m.d[0] ? m.d : "FT"); }
  }
  // scorers ticker
  char g[110] = "";
  if (m.h.goals[0]) snprintf(g, sizeof(g), "%s %s", m.h.a, m.h.goals);
  if (m.a.goals[0]) snprintf(g + strlen(g), sizeof(g) - strlen(g), "%s%s %s", g[0] ? "   " : "", m.a.a, m.a.goals);
  if (g[0]) { u8g2.setFont(F_XS); marquee(0, 63, 128, g, c.rt.pageAt); }
}
static bool fb_gesture(CardCtx& c, uint8_t g) {
  if (g != G_DOUBLE) return false;
  int n = fb_pages(c);
  if (n) c.rt.page = (c.rt.page + 1) % n;
  c.rt.pageAt = millis();
  return true;
}
static uint16_t fb_frame(CardCtx& c) {
  int idx[14];
  int n = min(fbPick(c, idx, 14), 8);
  if (!n) return 1000;
  const FbMatch& m = c.f.fb[idx[c.rt.page % n]];
  return (m.h.goals[0] || m.a.goals[0] || m.st == MS_IN) ? 50 : 500;
}
const CardImpl CI_FOOTBALL = {fb_draw, fb_gesture, fb_has, fb_frame, nullptr, fb_pages, fb_live, false};

// ================================================================== Cricket
static bool cr_has(CardCtx& c) { return c.f.ncr > 0; }
static int cr_pages(CardCtx& c) { return c.f.ncr; }
static bool cr_live(CardCtx& c) {
  for (int i = 0; i < c.f.ncr; i++) if (c.f.cr[i].st == MS_IN && c.f.cr[i].mine) return true;
  return false;
}

static void crRow(int y, const CrSide& s, bool live) {
  u8g2.setFont(F_L);
  u8g2.drawStr(0, y, s.a);
  int x = textW(s.a) + 3;
  if (live && s.bat) u8g2.drawTriangle(x, y - 8, x, y - 2, x + 4, y - 5);   // batting
  if (s.win) { u8g2.setFont(F_XS); u8g2.drawStr(x + (live && s.bat ? 6 : 0), y, "won"); }
  // score right aligned, overs in small type next to it
  char ov[12] = "";
  if (s.ov[0]) snprintf(ov, sizeof(ov), "(%s)", s.ov);
  u8g2.setFont(F_S);
  int ow = s.ov[0] ? textW(ov) : 0;
  if (ow) u8g2.drawStr(128 - ow, y, ov);
  u8g2.setFont(F_L);
  textR(128 - ow - (ow ? 3 : 0), y, s.sc[0] ? s.sc : "-");
}

static void cr_draw(CardCtx& c) {
  int p = autoPage(c, c.f.ncr, 7000);
  if (!c.f.ncr) {
    header(cardTitle(c.idx));
    emptyState("No matches", S.hubUrl[0] ? (S.nCrTeams ? "Your teams are not playing." : "Pick teams to follow in the dashboard.") : "Cricket needs the hub. Set it up in the dashboard.");
    return;
  }
  const CrMatch& m = c.f.cr[p];
  char title[32];
  snprintf(title, sizeof(title), "%s", m.ser[0] ? m.ser : m.ti);
  header(title, c.f.ncr, p);
  bool live = m.st == MS_IN;
  if (m.f[0]) {
    u8g2.setFont(F_XS);
    int w = textW(m.f) + 4;
    u8g2.drawFrame(128 - w, 12, w, 8);
    u8g2.drawStr(130 - w, 18, m.f);
  }
  if (m.st == MS_PRE) {
    u8g2.setFont(F_L);
    char vs[24];
    snprintf(vs, sizeof(vs), "%s  v  %s", m.s[0].a, m.s[1].a);
    textC(64, 33, vs);
    char when[24];
    fmtClock(when, sizeof(when), m.t, true);
    u8g2.setFont(F_MB);
    textC(64, 47, when);
    u8g2.setFont(F_XS);
    textFit(0, 62, 128, m.ti);
    return;
  }
  crRow(31, m.s[0], live);
  crRow(45, m.s[1], live);
  u8g2.setFont(F_S);
  if (m.status[0]) marquee(0, 55, 128, m.status, c.rt.pageAt);
  if (live) {
    char rr[40] = "";
    if (m.crr10) snprintf(rr, sizeof(rr), "CRR %d.%d", m.crr10 / 10, m.crr10 % 10);
    if (m.rrr10) snprintf(rr + strlen(rr), sizeof(rr) - strlen(rr), "%sRRR %d.%d", rr[0] ? "   " : "", m.rrr10 / 10, m.rrr10 % 10);
    u8g2.setFont(F_XS);
    u8g2.drawStr(0, 63, rr);
    textR(127, 63, "live");
    if ((millis() / 600) % 2) u8g2.drawDisc(127 - textW("live") - 5, 60, 2);
  }
}
static bool cr_gesture(CardCtx& c, uint8_t g) {
  if (g != G_DOUBLE || !c.f.ncr) return false;
  c.rt.page = (c.rt.page + 1) % c.f.ncr;
  c.rt.pageAt = millis();
  return true;
}
static uint16_t cr_frame(CardCtx&) { return 50; }
const CardImpl CI_CRICKET = {cr_draw, cr_gesture, cr_has, cr_frame, nullptr, cr_pages, cr_live, false};

// ================================================================== League table
static const Table* tbFor(CardCtx& c) {
  for (int i = 0; i < c.f.ntb; i++) if (!c.cfg.arg[0] || !strcmp(c.f.tb[i].lid, c.cfg.arg)) return &c.f.tb[i];
  return c.f.ntb ? &c.f.tb[0] : nullptr;
}
static bool tb_has(CardCtx& c) { const Table* t = tbFor(c); return t && t->n; }
static int tb_pages(CardCtx& c) { const Table* t = tbFor(c); return t ? (t->n + 4) / 5 : 0; }
static void tb_draw(CardCtx& c) {
  const Table* t = tbFor(c);
  int pages = tb_pages(c);
  int p = autoPage(c, pages, 5000);
  header(t && t->lg[0] ? t->lg : cardTitle(c.idx), pages, p);
  if (!t || !t->n) { emptyState("No table yet"); return; }
  u8g2.setFont(F_XS);
  u8g2.drawStr(0, 18, "#");
  u8g2.drawStr(16, 18, "team");
  textR(80, 18, "pl");
  textR(102, 18, "gd");
  textR(127, 18, "pts");
  for (int r = 0; r < 5; r++) {
    int i = p * 5 + r;
    if (i >= t->n) break;
    const TableRow& row = t->r[i];
    int y = 27 + r * 9;
    if (row.mine) { u8g2.drawBox(0, y - 8, 128, 9); u8g2.setDrawColor(0); }
    char b[8];
    u8g2.setFont(F_S);
    snprintf(b, sizeof(b), "%d", row.pos);
    textR(10, y, b);
    u8g2.setFont(F_MB);
    u8g2.drawStr(16, y, row.a);
    u8g2.setFont(F_S);
    snprintf(b, sizeof(b), "%d", row.gp);
    textR(80, y, b);
    snprintf(b, sizeof(b), "%+d", row.gd);
    textR(102, y, b);
    u8g2.setFont(F_MB);
    snprintf(b, sizeof(b), "%d", row.pts);
    textR(127, y, b);
    u8g2.setDrawColor(1);
  }
}
static bool tb_gesture(CardCtx& c, uint8_t g) {
  if (g != G_DOUBLE) return false;
  int n = tb_pages(c);
  if (n) c.rt.page = (c.rt.page + 1) % n;
  c.rt.pageAt = millis();
  return true;
}
static uint16_t tb_frame(CardCtx&) { return 1000; }
const CardImpl CI_TABLE = {tb_draw, tb_gesture, tb_has, tb_frame, nullptr, tb_pages, nullptr, false};
