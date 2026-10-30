// Host shim of the U8g2 C++ class on top of the U8g2 C library (no hardware).
#pragma once
#include <cstdint>
extern "C" {
#include "u8g2.h"
}
class U8G2_SSD1306_128X64_NONAME_F_HW_I2C {
 public:
  u8g2_t u;
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C(const u8g2_cb_t* r, uint8_t, uint8_t, uint8_t) {
    u8g2_Setup_ssd1306_128x64_noname_f(&u, r, u8x8_byte_empty, u8x8_dummy_cb);
    u8g2_InitDisplay(&u);
  }
  void begin() {}
  void setBusClock(uint32_t) {}
  void setFlipMode(uint8_t) {}
  void setContrast(uint8_t) {}
  void setPowerSave(uint8_t) {}
  void sendF(const char*, ...) {}
  void sendBuffer() {}
  void updateDisplayArea(uint8_t, uint8_t, uint8_t, uint8_t) {}
  void clearBuffer() { u8g2_ClearBuffer(&u); }
  uint8_t* getBufferPtr() { return u8g2_GetBufferPtr(&u); }
  void setFont(const uint8_t* f) { u8g2_SetFont(&u, f); }
  void setFontMode(uint8_t m) { u8g2_SetFontMode(&u, m); }
  void setFontPosBaseline() { u8g2_SetFontPosBaseline(&u); }
  void setDrawColor(uint8_t c) { u8g2_SetDrawColor(&u, c); }
  int drawStr(int x, int y, const char* s) { return u8g2_DrawStr(&u, x, y, s); }
  int getStrWidth(const char* s) { return u8g2_GetStrWidth(&u, s); }
  void drawPixel(int x, int y) { if (x >= 0 && y >= 0 && x < 128 && y < 64) u8g2_DrawPixel(&u, x, y); }
  void drawHLine(int x, int y, int w) { if (w > 0) u8g2_DrawHLine(&u, x, y, w); }
  void drawVLine(int x, int y, int h) { if (h > 0) u8g2_DrawVLine(&u, x, y, h); }
  void drawLine(int a, int b, int c, int d) { u8g2_DrawLine(&u, a, b, c, d); }
  void drawBox(int x, int y, int w, int h) { if (w > 0 && h > 0) u8g2_DrawBox(&u, x, y, w, h); }
  void drawFrame(int x, int y, int w, int h) { if (w > 0 && h > 0) u8g2_DrawFrame(&u, x, y, w, h); }
  void drawRBox(int x, int y, int w, int h, int r) { if (w > 0 && h > 0) u8g2_DrawRBox(&u, x, y, w, h, r); }
  void drawRFrame(int x, int y, int w, int h, int r) { u8g2_DrawRFrame(&u, x, y, w, h, r); }
  void drawCircle(int x, int y, int r, uint8_t o = U8G2_DRAW_ALL) { u8g2_DrawCircle(&u, x, y, r, o); }
  void drawDisc(int x, int y, int r, uint8_t o = U8G2_DRAW_ALL) { u8g2_DrawDisc(&u, x, y, r, o); }
  void drawTriangle(int a, int b, int c, int d, int e, int f) { u8g2_DrawTriangle(&u, a, b, c, d, e, f); }
  void setClipWindow(int a, int b, int c, int d) { u8g2_SetClipWindow(&u, a, b, c, d); }
  void setMaxClipWindow() { u8g2_SetMaxClipWindow(&u); }
};
