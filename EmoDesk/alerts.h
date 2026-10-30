#pragma once
#include <Arduino.h>

enum AlertKind : uint8_t {
  AK_MSG = 0, AK_NOTIFY, AK_GOAL, AK_VAR, AK_KICK, AK_FT, AK_WICKET, AK_START, AK_RESULT, AK_ALARM, AK_TIMER
};

// mood: +1 good news for us, -1 bad news, 0 neutral
void alertPush(uint8_t kind, const char* title, const char* text, int8_t mood = 0, uint16_t secs = 0, int8_t alarmIdx = -1);
uint8_t alertKindFromCode(uint16_t twoChars);   // hub kind "goal" -> 'g'|'o'<<8
bool alertActive();
void alertDraw();
bool alertGesture(uint8_t g);                    // true if consumed
void alertLoop();
uint32_t alertFrameMs();
