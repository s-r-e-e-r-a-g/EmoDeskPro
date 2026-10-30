#include "gfx.h"
#include "sys.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "feed.h"
#include "qrcode.h"
#include <Wire.h>
#include <math.h>
#include <time.h>

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);

static uint8_t sent[1024];        // what the panel currently shows
static uint8_t lastRaw[1024];     // previous frame before composition (slide source)
static uint8_t slideFrom[1024];
static uint8_t work[1024];
static bool haveSent = false, on = true;
static uint32_t slideAt = 0, frameSeq = 0;
static int8_t slideDir = 0;
static int8_t shiftX = 0, shiftY = 0;
static uint32_t shiftAt = 0;
static uint8_t curContrast = 0;
static const uint32_t SLIDE_MS = 240;

void gfxBegin() {
  u8g2.setBusClock(S.i2cHz ? S.i2cHz : OLED_I2C_HZ_DEFAULT);
  u8g2.begin();
  u8g2.setFontMode(1);
  u8g2.setFontPosBaseline();
  gfxApplySettings();
  memset(sent, 0, sizeof(sent));
}

void gfxApplySettings() {
  u8g2.setFlipMode(S.flip ? 1 : 0);
  haveSent = false;                       // flip changes every pixel: full refresh
}

void gfxContrast(uint8_t c) {
  if (c == curContrast) return;
  curContrast = c;
  u8g2.setContrast(c);
  // Lower pre-charge and VCOMH for a truly dim night mode; restore otherwise.
  if (c < 16) { u8g2.sendF("ca", 0xD9, 0x11); u8g2.sendF("ca", 0xDB, 0x00); }
  else { u8g2.sendF("ca", 0xD9, 0xF1); u8g2.sendF("ca", 0xDB, 0x40); }
}

void gfxPower(bool p) {
  if (p == on) return;
  on = p;
  u8g2.setPowerSave(p ? 0 : 1);
  if (p) haveSent = false;
}
bool gfxIsOn() { return on; }

void gfxClear() {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFontMode(1);
}

void gfxBeginSlide(int dir) {
  if (!S.slide) return;
  memcpy(slideFrom, lastRaw, sizeof(slideFrom));
  slideAt = millis() | 1;
  slideDir = dir >= 0 ? 1 : -1;
}

bool gfxAnimating() { return slideAt != 0; }

static float easeOut(float t) { t = 1 - t; return 1 - t * t * t; }

// Shift the whole frame by (dx, dy) pixels. Columns are 8 pages of 8 rows.
static void shiftFrame(uint8_t* b, int dx, int dy) {
  if (dy) {
    for (int x = 0; x < 128; x++) {
      uint64_t col = 0;
      for (int p = 0; p < 8; p++) col |= (uint64_t)b[p * 128 + x] << (p * 8);
      col = dy > 0 ? col << dy : col >> -dy;
      for (int p = 0; p < 8; p++) b[p * 128 + x] = (uint8_t)(col >> (p * 8));
    }
  }
  if (dx) {
    for (int p = 0; p < 8; p++) {
      uint8_t* row = b + p * 128;
      if (dx > 0) { memmove(row + dx, row, 128 - dx); memset(row, 0, dx); }
      else { memmove(row, row - dx, 128 + dx); memset(row + 128 + dx, 0, -dx); }
    }
  }
}

void gfxPresent() {
  uint8_t* buf = u8g2.getBufferPtr();
  memcpy(lastRaw, buf, 1024);
  uint32_t now = millis();

  if (slideAt) {
    float t = (now - slideAt) / (float)SLIDE_MS;
    if (t >= 1) slideAt = 0;
    else {
      int sh = (int)lroundf(128 * easeOut(t));
      for (int p = 0; p < 8; p++) {
        const uint8_t* o = slideFrom + p * 128;
        const uint8_t* n = buf + p * 128;
        uint8_t* w = work + p * 128;
        if (slideDir > 0) {
          for (int x = 0; x < 128; x++) w[x] = x < 128 - sh ? o[x + sh] : n[x - (128 - sh)];
        } else {
          for (int x = 0; x < 128; x++) w[x] = x < sh ? n[x + 128 - sh] : o[x - sh];
        }
      }
      memcpy(buf, work, 1024);
    }
  }

  // Burn-in protection: drift the whole image by up to 2 px every 3 minutes.
  if (S.burnShift) {
    if (now - shiftAt > 180000) {
      shiftAt = now;
      static const int8_t PATH[][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {2, 0}, {-2, 0}};
      static uint8_t step = 0;
      step = (step + 1) % (sizeof(PATH) / sizeof(PATH[0]));
      shiftX = PATH[step][0];
      shiftY = PATH[step][1];
    }
    if (shiftX || shiftY) shiftFrame(buf, shiftX, shiftY);
  }

  if (!on) return;
  if (!haveSent) {
    u8g2.sendBuffer();
    memcpy(sent, buf, 1024);
    haveSent = true;
    frameSeq++;
    return;
  }
  // Only send tiles that changed, one strip per page row.
  bool any = false;
  for (int ty = 0; ty < 8; ty++) {
    int first = -1, last = -1;
    for (int tx = 0; tx < 16; tx++) {
      int off = ty * 128 + tx * 8;
      if (memcmp(buf + off, sent + off, 8)) { if (first < 0) first = tx; last = tx; }
    }
    if (first >= 0) {
      u8g2.updateDisplayArea(first, ty, last - first + 1, 1);
      memcpy(sent + ty * 128 + first * 8, buf + ty * 128 + first * 8, (last - first + 1) * 8);
      any = true;
    }
  }
  if (any) frameSeq++;
}

const uint8_t* gfxFrame() { return sent; }
uint32_t gfxFrameSeq() { return frameSeq; }

// ------------------------------------------------------------------ text
int textW(const char* s) { return u8g2.getStrWidth(s); }
void textC(int cx, int y, const char* s) { u8g2.drawStr(cx - textW(s) / 2, y, s); }
void textR(int xr, int y, const char* s) { u8g2.drawStr(xr - textW(s), y, s); }

void textFit(int x, int y, int w, const char* s) {
  if (textW(s) <= w) { u8g2.drawStr(x, y, s); return; }
  char b[128];
  safeCopy(b, s, sizeof(b));
  int n = strlen(b);
  int dots = textW("..");
  while (n > 0 && (b[n] = 0, textW(b) + dots > w)) n--;
  b[n] = 0;
  while (n > 0 && b[n - 1] == ' ') b[--n] = 0;
  strlcat(b, "..", sizeof(b));
  u8g2.drawStr(x, y, b);
}

// Greedy word wrap. Returns number of lines drawn.
int textWrap(int x, int y, int w, int lineH, const char* s, int maxLines) {
  int lines = 0;
  const char* p = s;
  char line[96];
  while (*p && lines < maxLines) {
    while (*p == ' ') p++;
    int len = 0, lastSpace = -1;
    const char* q = p;
    while (*q) {
      if (len >= (int)sizeof(line) - 1) break;
      line[len] = *q;
      line[len + 1] = 0;
      if (textW(line) > w) break;
      if (*q == ' ') lastSpace = len;
      len++;
      q++;
    }
    bool more = *q != 0;
    if (more && lastSpace > 0) len = lastSpace;
    line[len] = 0;
    if (lines == maxLines - 1 && more) {      // last allowed line: ellipsis
      char tail[128];
      safeCopy(tail, p, sizeof(tail));
      textFit(x, y + lines * lineH, w, tail);
      return lines + 1;
    }
    u8g2.drawStr(x, y + lines * lineH, line);
    lines++;
    p += len;
    if (!len) p++;   // a single glyph wider than w: skip it
  }
  return lines;
}

void marquee(int x, int y, int w, const char* s, uint32_t t0) {
  int tw = textW(s);
  if (tw <= w) { u8g2.drawStr(x, y, s); return; }
  // pause 1.2 s at the start, scroll at 28 px/s, pause at the end, repeat
  int span = tw - w;
  uint32_t scrollMs = span * 1000 / 28;
  uint32_t cycle = 1200 + scrollMs + 1200;
  uint32_t t = (millis() - t0) % cycle;
  int off = t < 1200 ? 0 : t < 1200 + scrollMs ? (int)((t - 1200) * 28 / 1000) : span;
  u8g2.setClipWindow(x, 0, x + w, 64);   // x1, y1 are exclusive
  u8g2.drawStr(x - off, y, s);
  u8g2.setMaxClipWindow();
}

// ------------------------------------------------------------------ shapes
void dotLine(int x, int y, int w) { for (int i = 0; i < w; i += 2) u8g2.drawPixel(x + i, y); }

void bar(int x, int y, int w, int h, float frac) {
  frac = constrain(frac, 0.0f, 1.0f);
  u8g2.drawFrame(x, y, w, h);
  int fw = (int)((w - 2) * frac + 0.5f);
  if (fw > 0) u8g2.drawBox(x + 1, y + 1, fw, h - 2);
}

void spark(int x, int y, int w, int h, const uint8_t* v, int n, int maxV) {
  if (n < 2) return;
  int px = x, py = y + h - 1 - v[0] * (h - 1) / maxV;
  for (int i = 1; i < n; i++) {
    int nx = x + i * (w - 1) / (n - 1);
    int ny = y + h - 1 - v[i] * (h - 1) / maxV;
    u8g2.drawLine(px, py, nx, ny);
    px = nx;
    py = ny;
  }
}

void pageDots(int xr, int y, int n, int cur) {
  if (n < 2) return;
  if (n > 8) n = 8;
  int x = xr - (n - 1) * 4;
  for (int i = 0; i < n; i++) {
    if (i == cur) u8g2.drawBox(x + i * 4 - 1, y - 1, 3, 3);
    else u8g2.drawPixel(x + i * 4, y);
  }
}

void degree(int x, int y) { u8g2.drawCircle(x + 1, y + 1, 1); }

void wifiIcon(int x, int y, int rssi) {
  // three bars, filled by signal strength; hollow when offline
  int lvl = rssi == 0 ? 0 : rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
  for (int i = 0; i < 3; i++) {
    int h = 3 + i * 2;
    if (i < lvl) u8g2.drawBox(x + i * 3, y + 7 - h, 2, h);
    else u8g2.drawPixel(x + i * 3, y + 6);
  }
}

void iconBall(int cx, int cy, int r, uint8_t fg) {
  u8g2.setDrawColor(fg);
  u8g2.drawDisc(cx, cy, r);
  u8g2.setDrawColor(!fg);
  u8g2.drawDisc(cx, cy, r - 1);
  u8g2.setDrawColor(fg);
  // pentagon patch + seams
  int pr = max(1, r / 3);
  u8g2.drawDisc(cx, cy, pr);
  for (int i = 0; i < 5; i++) {
    float a = -M_PI / 2 + i * 2 * M_PI / 5;
    u8g2.drawLine(cx + cosf(a) * pr, cy + sinf(a) * pr, cx + cosf(a) * (r - 1), cy + sinf(a) * (r - 1));
  }
  u8g2.setDrawColor(1);
}

void iconBell(int x, int y) {   // 11x12
  u8g2.drawDisc(x + 5, y + 4, 4, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2.drawBox(x + 1, y + 4, 9, 4);
  u8g2.drawBox(x, y + 8, 11, 2);
  u8g2.drawBox(x + 4, y + 10, 3, 2);
  u8g2.drawPixel(x + 5, y);
}

void iconMail(int x, int y) {   // 13x9
  u8g2.drawFrame(x, y, 13, 9);
  u8g2.drawLine(x, y, x + 6, y + 5);
  u8g2.drawLine(x + 12, y, x + 6, y + 5);
}

void iconBat(int x, int y, int pct) {  // 12x6
  u8g2.drawFrame(x, y, 11, 6);
  u8g2.drawBox(x + 11, y + 2, 1, 2);
  int w = constrain(pct, 0, 100) * 9 / 100;
  if (w > 0) u8g2.drawBox(x + 1, y + 1, w, 4);
}

// ------------------------------------------------------------------ weather icons
static int wxClass(int c) {
  if (c == 0 || c == 1) return 0;                     // clear
  if (c == 2) return 1;                                // partly cloudy
  if (c == 3) return 2;                                // overcast
  if (c == 45 || c == 48) return 3;                    // fog
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return 5;  // snow
  if (c >= 95) return 6;                               // thunder
  if (c >= 51) return 4;                               // drizzle / rain / showers
  return 2;
}

const char* wxText(int c) {
  switch (c) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Storm, hail";
    default: return "";
  }
}

static void cloud(int cx, int cy, int s) {
  // s = scale in px of the main puff radius
  u8g2.drawDisc(cx - s, cy, s * 2 / 3 + 1);
  u8g2.drawDisc(cx + s, cy, s * 2 / 3 + 1);
  u8g2.drawDisc(cx, cy - s / 2, s);
  u8g2.drawBox(cx - s, cy, 2 * s + 1, s * 2 / 3 + 2);
  // hollow it to an outline for a lighter look
  u8g2.setDrawColor(0);
  int r1 = s * 2 / 3, r2 = s - 1;
  u8g2.drawDisc(cx - s, cy, r1);
  u8g2.drawDisc(cx + s, cy, r1);
  u8g2.drawDisc(cx, cy - s / 2, r2);
  u8g2.drawBox(cx - s, cy, 2 * s + 1, s * 2 / 3 + 1);
  u8g2.setDrawColor(1);
}

static void sun(int cx, int cy, int r, bool rays) {
  u8g2.drawCircle(cx, cy, r);
  if (!rays) return;
  for (int i = 0; i < 8; i++) {
    float a = i * M_PI / 4;
    u8g2.drawLine(cx + cosf(a) * (r + 2), cy + sinf(a) * (r + 2), cx + cosf(a) * (r + 2 + r / 2 + 1), cy + sinf(a) * (r + 2 + r / 2 + 1));
  }
}

static void moon(int cx, int cy, int r) {
  u8g2.drawDisc(cx, cy, r);
  u8g2.setDrawColor(0);
  u8g2.drawDisc(cx + r / 2 + 1, cy - r / 3, r - 1);
  u8g2.setDrawColor(1);
}

void wxIcon(int cx, int cy, int size, int code, bool day) {
  int k = wxClass(code);
  int s = size ? 9 : 5;
  switch (k) {
    case 0:
      if (day) sun(cx, cy, size ? 7 : 4, true);
      else moon(cx, cy, size ? 9 : 5);
      break;
    case 1:
      if (day) sun(cx + s - 1, cy - s + 1, size ? 5 : 3, true);
      else moon(cx + s, cy - s, size ? 6 : 3);
      cloud(cx - 1, cy + s / 2, s);
      break;
    case 3:
      for (int i = 0; i < 4; i++) u8g2.drawHLine(cx - s - 2 + (i & 1) * 2, cy - s + 2 + i * (size ? 5 : 3), 2 * s + 2);
      break;
    default: {
      cloud(cx, cy - (size ? 3 : 2), s);
      int by = cy + (size ? 8 : 4);
      if (k == 4) for (int i = -1; i <= 1; i++) u8g2.drawLine(cx + i * (s - 1), by, cx + i * (s - 1) - 2, by + (size ? 5 : 3));
      if (k == 5) for (int i = -1; i <= 1; i++) { u8g2.drawPixel(cx + i * (s - 1), by + 1); u8g2.drawPixel(cx + i * (s - 1) - 1, by + (size ? 5 : 3)); }
      if (k == 6) { u8g2.drawLine(cx + 1, by - 1, cx - 2, by + 3); u8g2.drawLine(cx - 2, by + 3, cx + 2, by + 3); u8g2.drawLine(cx + 2, by + 3, cx - 1, by + (size ? 8 : 6)); }
      break;
    }
  }
}

// ------------------------------------------------------------------ QR
void qrCode(int x, int y, const char* text, int scale) {
  QRCode qr;
  uint8_t data[qrcode_getBufferSize(4)];
  int ok = -1;
  for (int v = 2; v <= 4 && ok != 0; v++) ok = qrcode_initText(&qr, data, v, ECC_LOW, text);   // smallest that fits
  if (ok != 0) return;
  int px = qr.size * scale;
  u8g2.setDrawColor(1);
  u8g2.drawBox(x - 2, y - 2, px + 4, px + 4);   // quiet zone (inverted)
  u8g2.setDrawColor(0);
  for (int yy = 0; yy < qr.size; yy++)
    for (int xx = 0; xx < qr.size; xx++)
      if (qrcode_getModule(&qr, xx, yy)) u8g2.drawBox(x + xx * scale, y + yy * scale, scale, scale);
  u8g2.setDrawColor(1);
}

// ------------------------------------------------------------------ header
void header(const char* title, int pages, int page) {
  u8g2.setFont(F_MB);
  textFit(0, 8, 70, title);
  if (pages > 1) pageDots(min(70, textW(title)) + 4 + (pages - 1) * 4, 5, pages, page);
  // right side: hub/alert state, wi-fi, time
  int xr = 127;
  struct tm t;
  u8g2.setFont(F_S);
  if (localNow(t)) {
    char b[10];
    fmtTime(b, sizeof(b), t.tm_hour, t.tm_min, S.h24, false);
    textR(xr, 8, b);
    xr -= textW(b) + 3;
  }
  wifiIcon(xr - 8, 1, wifiRssi());
  xr -= 11;
  HubState h = hubState();
  if (S.hubUrl[0] && h == HUB_ERROR) {   // hub unreachable: small hollow ring
    u8g2.drawCircle(xr - 2, 5, 2);
    xr -= 6;
  }
  dotLine(0, 10, 128);
}

void emptyState(const char* l1, const char* l2) {
  u8g2.setFont(F_MB);
  textC(64, l2 ? 34 : 40, l1);
  if (l2) {
    u8g2.setFont(F_M);
    textWrap(6, 48, 116, 10, l2, 2);
  }
}
