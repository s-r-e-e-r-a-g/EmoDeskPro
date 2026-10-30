#include "input.h"
#include "sys.h"
#include "settings.h"

struct Btn {
  uint8_t pin;
  bool activeHigh;
  bool stable, raw;
  uint32_t rawAt, downAt, upAt;
  uint8_t taps;
  bool longFired;
};

static Btn btn[2] = {{PIN_TOUCH, true}, {PIN_BOOT, false}};
static uint8_t qG[16], qS[16];
static uint8_t qHead = 0, qTail = 0;
static uint32_t lastAct = 0;

static void push(uint8_t g, uint8_t src) {
  uint8_t n = (qHead + 1) & 15;
  if (n == qTail) return;      // full: drop
  qG[qHead] = g;
  qS[qHead] = src;
  qHead = n;
  lastAct = millis();
}

void inputBegin() {
  pinMode(PIN_TOUCH, INPUT);   // TTP223 drives the line
  pinMode(PIN_BOOT, INPUT_PULLUP);
  for (auto& b : btn) {
    b.raw = b.stable = false;
    b.rawAt = b.downAt = b.upAt = 0;
    b.taps = 0;
  }
  lastAct = millis();
}

void inputPoll() {
  uint32_t now = millis();
  for (int i = 0; i < 2; i++) {
    Btn& b = btn[i];
    bool r = digitalRead(b.pin) == (b.activeHigh ? HIGH : LOW);
    if (r != b.raw) { b.raw = r; b.rawAt = now; }
    if (r != b.stable && now - b.rawAt >= 15) {   // 15 ms debounce
      b.stable = r;
      if (r) {
        b.downAt = now;
        b.longFired = false;
        push(G_DOWN, i);
      } else {
        uint32_t held = now - b.downAt;
        push(G_UP, i);
        if (b.longFired) b.taps = 0;
        else if (held >= S.holdMs) { b.taps = 0; push(G_HOLD, i); }
        else { b.taps++; b.upAt = now; }
      }
    }
    if (b.stable && !b.longFired && now - b.downAt >= S.longMs) {
      b.longFired = true;
      b.taps = 0;
      push(G_LONG, i);
    }
    if (!b.stable && b.taps && now - b.upAt >= S.multiMs) {
      push(b.taps >= 3 ? G_TRIPLE : b.taps == 2 ? G_DOUBLE : G_TAP, i);
      b.taps = 0;
    }
  }
}

bool inputGet(uint8_t& g, uint8_t& src) {
  if (qTail == qHead) return false;
  g = qG[qTail];
  src = qS[qTail];
  qTail = (qTail + 1) & 15;
  return true;
}

bool inputHeld() { return btn[0].stable; }
uint32_t inputHeldMs() { return btn[0].stable ? millis() - btn[0].downAt : 0; }
uint32_t inputLastMs() { return (btn[0].stable || btn[1].stable) ? millis() : lastAct; }
void inputInject(uint8_t g) { push(g, SRC_WEB); }

const char* gestureName(uint8_t g) {
  static const char* const N[] = {"none", "tap", "double", "triple", "hold", "long", "down", "up"};
  return g < 8 ? N[g] : "?";
}
