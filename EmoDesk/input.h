#pragma once
#include <Arduino.h>

// Gestures from the touch pad (and the BOOT button).
//   tap / double / triple : fired after the multi-tap window closes
//   hold                  : fired on release, held 0.6 s .. 2.5 s
//   long                  : fired while still held, at 2.5 s
//   down / up             : raw edges (games use DOWN for zero-latency jumps)
enum Gesture : uint8_t { G_NONE = 0, G_TAP, G_DOUBLE, G_TRIPLE, G_HOLD, G_LONG, G_DOWN, G_UP };
enum { SRC_TOUCH = 0, SRC_BOOT = 1, SRC_WEB = 2 };

void inputBegin();
void inputPoll();                         // call every loop
bool inputGet(uint8_t& g, uint8_t& src);
bool inputHeld();                         // touch pad currently pressed
uint32_t inputHeldMs();                   // how long (0 if not)
uint32_t inputLastMs();                   // millis() of the last activity
void inputInject(uint8_t g);              // virtual pad from the dashboard
const char* gestureName(uint8_t g);
