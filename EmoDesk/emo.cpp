// Emo's face: two rounded-rectangle eyes driven by a handful of parameters
// (size, corner radius, look offset, lids). Every frame the parameters ease
// towards the target expression, so moods morph smoothly instead of jumping.
#include "emo.h"
#include "gfx.h"
#include "sys.h"
#include "settings.h"
#include "input.h"
#include <math.h>

struct Eye { float w, h, r, lidT, slant, lidB; };

static const char* const NAMES[M_COUNT] = {"neutral", "happy", "sad", "angry", "surprised", "sleepy", "love", "excited", "wink", "dizzy", "sleep", "focus"};

// target expressions: w, h, radius, top lid (0..1), slant (+angry / -sad), bottom lid (happy)
static const Eye PRESET[M_COUNT] = {
  {28, 30, 8, 0.00f, 0.00f, 0.00f},   // neutral
  {28, 28, 9, 0.00f, 0.00f, 0.52f},   // happy
  {26, 26, 7, 0.10f, -0.55f, 0.00f},  // sad
  {28, 26, 6, 0.08f, 0.60f, 0.00f},   // angry
  {32, 36, 15, 0.00f, 0.00f, 0.00f},  // surprised
  {28, 30, 8, 0.55f, 0.00f, 0.00f},   // sleepy
  {28, 30, 8, 0.00f, 0.00f, 0.00f},   // love (hearts drawn instead)
  {30, 32, 10, 0.00f, 0.00f, 0.45f},  // excited
  {28, 30, 8, 0.00f, 0.00f, 0.00f},   // wink (right eye closed)
  {26, 26, 13, 0.00f, 0.00f, 0.00f},  // dizzy (spirals)
  {28, 3, 1, 0.00f, 0.00f, 0.00f},    // sleep (closed)
  {30, 16, 5, 0.00f, 0.20f, 0.00f},   // focus
};

static Eye cur = PRESET[M_NEUTRAL];
static float lookX = 0, lookY = 0, tgtX = 0, tgtY = 0;
static float blink = 0;                 // 0 open .. 1 closed
static uint32_t blinkAt = 0, blinkStart = 0, lookAt = 0, moodUntil = 0, lastUpd = 0, wakeUntil = 0;
static bool doubleBlink = false;
static uint8_t mood = M_NEUTRAL, base = M_NEUTRAL;
static uint8_t pokes = 0;
static uint32_t pokeAt = 0;
static bool petting = false;
static uint32_t petStart = 0;

void emoBegin() {
  lastUpd = millis();
  blinkAt = millis() + 2500;
  lookAt = millis() + 2000;
}

int moodFromName(const char* s) {
  if (!s) return -1;
  for (int i = 0; i < M_COUNT; i++) if (!strcasecmp(s, NAMES[i])) return i;
  return -1;
}
const char* moodName(uint8_t m) { return m < M_COUNT ? NAMES[m] : "neutral"; }
uint8_t emoMood() { return mood; }

void emoSet(uint8_t m, uint32_t ms) {
  if (m >= M_COUNT) return;
  mood = m;
  moodUntil = millis() + ms;
}

void emoBase(uint8_t m) { if (m < M_COUNT) base = m; }

void emoWake() {
  wakeUntil = millis() + 60000;
  if (mood == M_SLEEP || mood == M_SLEEPY) emoSet(M_SURPRISED, 700);
}

void emoPoke() {
  emoWake();
  uint32_t now = millis();
  pokes = now - pokeAt < 4000 ? pokes + 1 : 1;
  pokeAt = now;
  if (pokes >= 4) { emoSet(M_DIZZY, 2600); pokes = 0; }
  else if (pokes == 3) emoSet(M_ANGRY, 1500);
  else { emoSet(M_SURPRISED, 600); tgtX = (esp_random() % 2 ? 8 : -8); tgtY = -3; }
}

void emoPet(bool holding) {
  if (holding && !petting) { petStart = millis(); emoWake(); }
  petting = holding;
  if (holding) emoSet(M_LOVE, 1500);
  else if (mood == M_LOVE) emoSet(M_HAPPY, 2500);
}

static uint8_t restingMood() {
  if (millis() < wakeUntil) return base;
  int m = minuteOfDay();
  if (m >= 0 && inWindow(m, parseHM(S.sleepAt), parseHM(S.wakeAt))) return M_SLEEP;
  if (millis() - inputLastMs() > 15UL * 60000) return M_SLEEPY;   // bored
  return base;
}

static float approach(float c, float t, float k) { return c + (t - c) * k; }

void emoUpdate() {
  uint32_t now = millis();
  float dt = (now - lastUpd) / 1000.0f;
  lastUpd = now;
  if (dt > 0.2f) dt = 0.2f;
  if (moodUntil && (int32_t)(now - moodUntil) >= 0) { moodUntil = 0; mood = restingMood(); }
  if (!moodUntil) mood = restingMood();

  float lively = 0.4f + S.lively / 100.0f;   // 0.4 .. 1.4
  // wandering gaze
  if ((int32_t)(now - lookAt) >= 0 && mood != M_SLEEP) {
    if (esp_random() % 3 == 0) { tgtX = 0; tgtY = 0; }
    else { tgtX = (int)(esp_random() % 19) - 9; tgtY = (int)(esp_random() % 11) - 5; }
    lookAt = now + (uint32_t)((1200 + esp_random() % 3800) / lively);
  }
  if (mood == M_SLEEP || mood == M_SLEEPY) { tgtX = 0; tgtY = 3; }
  if (mood == M_FOCUS) { tgtX = 0; tgtY = 2; }
  // blinking
  if (!blinkStart && (int32_t)(now - blinkAt) >= 0 && mood != M_SLEEP) {
    blinkStart = now;
    doubleBlink = esp_random() % 5 == 0;
  }
  if (blinkStart) {
    uint32_t t = now - blinkStart;
    uint32_t len = doubleBlink ? 360 : 160;
    if (t >= len) { blinkStart = 0; blink = 0; blinkAt = now + (uint32_t)((1800 + esp_random() % 4200) / lively); }
    else {
      uint32_t ph = t % 180;
      blink = ph < 80 ? ph / 80.0f : ph < 100 ? 1 : 1 - (ph - 100) / 80.0f;
    }
  }
  // ease towards the target shape
  const Eye& T = PRESET[mood];
  float k = 1 - expf(-dt / 0.07f);
  cur.w = approach(cur.w, T.w, k);
  cur.h = approach(cur.h, T.h, k);
  cur.r = approach(cur.r, T.r, k);
  cur.lidT = approach(cur.lidT, T.lidT, k);
  cur.slant = approach(cur.slant, T.slant, k);
  cur.lidB = approach(cur.lidB, T.lidB, k);
  float kl = 1 - expf(-dt / 0.09f);
  lookX = approach(lookX, tgtX, kl);
  lookY = approach(lookY, tgtY, kl);
}

static void heart(int cx, int cy, int s) {
  u8g2.drawDisc(cx - s / 2, cy - s / 4, s / 2 + 1);
  u8g2.drawDisc(cx + s / 2, cy - s / 4, s / 2 + 1);
  u8g2.drawTriangle(cx - s - 1, cy - s / 8, cx + s + 1, cy - s / 8, cx, cy + s);
}

static void spiral(int cx, int cy, int r, float rot) {
  float px = cx, py = cy;
  for (float a = 0; a < 4 * M_PI; a += 0.35f) {
    float rr = r * a / (4 * M_PI);
    float x = cx + cosf(a + rot) * rr, y = cy + sinf(a + rot) * rr;
    u8g2.drawLine(px, py, x, y);
    px = x;
    py = y;
  }
}

// One eye. side = -1 left, +1 right (slant mirrors so the inner corners move).
static void drawEye(int cx, int cy, float sc, int side, bool closed) {
  float w = cur.w * sc, h = cur.h * sc;
  float hb = h * (1 - blink * 0.92f);
  if (closed) hb = 3 * sc;
  if (hb < 2) hb = 2;
  int x = (int)(cx - w / 2), y = (int)(cy - hb / 2);
  int r = (int)min(cur.r * sc, min(w, hb) / 2 - 0.5f);
  if (r < 0) r = 0;
  u8g2.drawRBox(x, y, (int)w, (int)hb, r);
  u8g2.setDrawColor(0);
  // top lid: straight cut (sleepy) plus a slanted cut (angry / sad)
  if (cur.lidT > 0.02f) u8g2.drawBox(x - 1, y - 1, (int)w + 2, (int)(hb * cur.lidT) + 1);
  if (fabsf(cur.slant) > 0.03f) {
    float d = hb * fabsf(cur.slant) * 0.7f;
    // angry: inner corner (towards the nose) is lower; sad: outer corner lower
    bool innerLow = (cur.slant > 0);
    int xi = side < 0 ? x + (int)w + 1 : x - 1;     // inner edge x
    int xo = side < 0 ? x - 1 : x + (int)w + 1;     // outer edge x
    int yTop = y - 1;
    if (innerLow) u8g2.drawTriangle(xo, yTop, xi, yTop, xi, yTop + (int)d);
    else u8g2.drawTriangle(xi, yTop, xo, yTop, xo, yTop + (int)d);
  }
  // bottom lid: a big black circle pushing up = smiling eyes
  if (cur.lidB > 0.02f) {
    float R = w * 0.8f;
    int ccy = (int)(y + hb + R - hb * cur.lidB);
    u8g2.drawDisc(cx, ccy, (int)R);
  }
  u8g2.setDrawColor(1);
}

void emoDraw(int cx, int cy, float sc) {
  uint32_t now = millis();
  float breathe = sinf(now / 640.0f) * 0.9f;
  int lx = (int)(lookX * sc), ly = (int)((lookY + breathe) * sc);
  int gap = (int)(24 * sc);
  int ex[2] = {cx - gap + lx, cx + gap + lx};
  int ey = cy + ly;

  if (mood == M_LOVE) {
    int s = (int)(12 * sc + sinf(now / 120.0f) * 1.5f);
    heart(ex[0], ey, s);
    heart(ex[1], ey, s);
    // floating hearts while petted
    if (petting) {
      for (int i = 0; i < 3; i++) {
        uint32_t t = (now - petStart + i * 600) % 1800;
        int hx = cx + (i - 1) * 30 + (int)(sinf(t / 200.0f + i) * 3);
        int hy = cy + 20 - (int)(t * 40 / 1800);
        if (hy > 0) heart(hx, hy, 3);
      }
    }
    return;
  }
  if (mood == M_DIZZY) {
    float rot = now / 90.0f;
    spiral(ex[0], ey, (int)(12 * sc), rot);
    spiral(ex[1], ey, (int)(12 * sc), rot);
    return;
  }
  drawEye(ex[0], ey, sc, -1, false);
  drawEye(ex[1], ey, sc, 1, mood == M_WINK);
  if (mood == M_SLEEP) {
    // drifting "z z Z"
    u8g2.setFont(F_S);
    uint32_t t = now % 3000;
    for (int i = 0; i < 3; i++) {
      int ph = (t + i * 1000) % 3000;
      int zx = cx + 34 + i * 6, zy = cy - 4 - ph * 22 / 3000;
      if (zy > 2) u8g2.drawStr(zx, zy, i == 2 ? "Z" : "z");
    }
  }
}
