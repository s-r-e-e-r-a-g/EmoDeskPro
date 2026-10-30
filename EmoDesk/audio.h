#pragma once
#include <Arduino.h>

// Optional MAX98357A speaker. Small synth (sine + soft envelope) playing named
// effects and short melodies in its own task. Nothing runs, and no RAM is
// used, until "Speaker installed" is switched on.
void audioBegin();
void audioApply();                                   // settings changed
bool audioPlay(const char* name, uint8_t event, bool loop = false);   // event = SoundEvent
void audioStop();
bool audioBusy();
const char* const* audioNames(int& n);
