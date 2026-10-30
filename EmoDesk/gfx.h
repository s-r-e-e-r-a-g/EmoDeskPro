#pragma once
// Display layer: U8g2 frame buffer, a compositor (slide transitions, burn-in
// shift), and a flush that only sends the 8x8 tiles that changed.
//
// Cards draw into the full 128x64 buffer with ordinary U8g2 calls. The
// compositor works on the finished buffer, so transitions and pixel shift need
// no special code in any card.
#include <U8g2lib.h>

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

// Fonts (all ASCII "_tr" variants; the hub folds text to ASCII)
#define F_XS    u8g2_font_4x6_tr          // 6 px, tiny labels
#define F_S     u8g2_font_5x8_tr          // 8 px, status bar / small text
#define F_M     u8g2_font_helvR08_tr      // body text
#define F_MB    u8g2_font_helvB08_tr      // body bold
#define F_L     u8g2_font_helvB10_tr      // headings, team codes
#define F_XL    u8g2_font_helvB14_tr
#define F_N18   u8g2_font_logisoso18_tn   // numbers only
#define F_N22   u8g2_font_logisoso22_tn
#define F_N32   u8g2_font_logisoso32_tn
#define F_N42   u8g2_font_logisoso42_tn

#define CONTENT_Y 12                      // first row below the status bar

void gfxBegin();
void gfxApplySettings();                  // contrast, flip, bus clock
void gfxPower(bool on);
bool gfxIsOn();
void gfxContrast(uint8_t c);

// Frame lifecycle (UI loop):  gfxClear(); ...draw...; gfxPresent();
void gfxClear();
void gfxPresent();                        // compositor + diff flush + mirror
const uint8_t* gfxFrame();                // last presented frame (for mirror)
uint32_t gfxFrameSeq();                   // bumps when the visible frame changed

// Slide transition: call before drawing the new screen. dir +1 = new comes from the right.
void gfxBeginSlide(int dir);
bool gfxAnimating();

// ---------- drawing helpers ----------
int  textW(const char* s);
void textC(int cx, int y, const char* s);                 // centred at cx
void textR(int xr, int y, const char* s);                 // right aligned at xr
void textFit(int x, int y, int w, const char* s);         // clip with "..." if too wide
int  textWrap(int x, int y, int w, int lineH, const char* s, int maxLines);   // returns lines used
void marquee(int x, int y, int w, const char* s, uint32_t t0);               // scrolls if too wide
void dotLine(int x, int y, int w);
void bar(int x, int y, int w, int h, float frac);
void spark(int x, int y, int w, int h, const uint8_t* v, int n, int maxV);
void pageDots(int xr, int y, int n, int cur);
void degree(int x, int y);                                // small degree ring, top-left at x,y
void wifiIcon(int x, int y, int rssi);                    // 9x7
void iconBall(int cx, int cy, int r, uint8_t fg = 1);   // fg 0 = draw on a white band
void iconBell(int x, int y);
void iconMail(int x, int y);
void iconBat(int x, int y, int pct);
void wxIcon(int cx, int cy, int size, int code, bool day); // size: 0 small (16px), 1 big (28px)
const char* wxText(int code);
void qrCode(int x, int y, const char* text, int scale);

// Standard card header: title left, time + status icons right, dotted rule.
void header(const char* title, int pages = 0, int page = 0);
void emptyState(const char* line1, const char* line2 = nullptr);
