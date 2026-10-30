#pragma once
// Card engine: rotation, navigation, gestures, per-card runtime state.
#include <Arduino.h>
#include "settings.h"
#include "feed.h"

struct CardRt {
  uint8_t page;
  uint32_t pageAt;
  uint32_t a, b, c, d;       // per-type state (timers, game, ...)
  bool running;
};

struct CardCtx {
  int idx;
  const CardCfg& cfg;
  CardRt& rt;
  uint32_t t0;               // millis() when the card came on screen
  const Feed& f;             // valid while drawing (feed is locked)
};

struct CardImpl {
  void (*draw)(CardCtx&);
  bool (*gesture)(CardCtx&, uint8_t g);     // double / hold / down; true if consumed
  bool (*hasData)(CardCtx&);                // false = empty, skipped in rotation
  uint16_t (*frameMs)(CardCtx&);            // redraw interval while visible
  void (*tick)(CardCtx&);                   // every loop, visible or not (timers)
  int (*pages)(CardCtx&);                   // pages shown in rotation (auto-advance)
  bool (*live)(CardCtx&);                   // live content: stays twice as long
  bool raw;                                  // game: gets DOWN events, no rotation
};

extern const CardImpl* const CARD_IMPL[CT_COUNT];

void cardsBegin();
void cardsLoop();                 // rotation, ticks
void cardsDraw();                 // draw the visible card (call between gfxClear / gfxPresent)
uint16_t cardsFrameMs();
bool cardsGesture(uint8_t g);     // double/hold/down/up for the current card
void cardsNext(int dir);
void cardsGoto(int i);
void cardsHome();
void cardsPinToggle();
bool cardsPinned();
void cardsChanged();              // card list edited
int  cardsCurrent();
const char* cardsCurrentTitle();
bool cardsCurrentRaw();
const char* cardTitle(int i);

// helpers for card implementations
int  autoPage(CardCtx& c, int n, uint32_t everyMs = 5000);
void fmtTemp(char* b, size_t n, int16_t t10);
void fmtAgo(char* b, size_t n, uint32_t sinceMs);
void fmtClock(char* b, size_t n, uint32_t unixT, bool withDay);
void fmtDay(char* b, size_t n, uint32_t unixT);   // "Today", "Tomorrow", "Sat", "5 Nov"

// card implementations (cards_*.cpp)
extern const CardImpl CI_EMO, CI_CLOCK, CI_ANALOG, CI_WEATHER, CI_FORECAST, CI_AIR, CI_CRICKET, CI_FOOTBALL,
    CI_TABLE, CI_MARKETS, CI_NEWS, CI_QUOTE, CI_AGENDA, CI_POMODORO, CI_TIMER, CI_STOPWATCH, CI_ALARMS, CI_PC,
    CI_MESSAGES, CI_SYSTEM, CI_DINO;

// alarms engine (cards_time.cpp)
void alarmsLoop();
bool focusActive();
