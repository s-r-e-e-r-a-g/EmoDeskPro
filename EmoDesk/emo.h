#pragma once
#include <Arduino.h>

enum Mood : uint8_t {
  M_NEUTRAL = 0, M_HAPPY, M_SAD, M_ANGRY, M_SURPRISED, M_SLEEPY, M_LOVE, M_EXCITED, M_WINK,
  M_DIZZY, M_SLEEP, M_FOCUS, M_COUNT
};

void emoBegin();
void emoUpdate();                          // every UI loop: tweening, blinks, idle behaviour
void emoDraw(int cx, int cy, float scale); // the face, centred
void emoSet(uint8_t mood, uint32_t ms);    // temporary mood, then back to the base mood
void emoBase(uint8_t mood);                // resting mood (e.g. focus mode)
void emoPoke();
void emoPet(bool holding);
void emoWake();                            // any touch: wake up if napping
uint8_t emoMood();
int  moodFromName(const char* s);
const char* moodName(uint8_t m);
