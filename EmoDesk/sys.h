#pragma once
// Core services shared by every module: logging, time, the command bus that
// carries requests from the web/net tasks to the UI loop, and small helpers.
//
// Threading model (the main fix for the old freezes):
//  * The UI loop (Arduino loop) owns the display, input, cards, settings and
//    alerts. Nothing else touches them directly.
//  * The web server (AsyncTCP task) and the net task never draw and never
//    mutate settings. They post Cmds to the bus, which the UI drains each loop.
//  * Shared read-mostly data (feed, settings snapshot for the net task) sits
//    behind short-held mutexes. Nobody holds a lock while doing I/O.
#include <Arduino.h>
#include "config.h"

// ---------- logging ----------
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
size_t logCopy(char* out, size_t n);        // newest last, NUL-terminated

// ---------- time ----------
bool   timeValid();                         // NTP synced at least once
bool   localNow(struct tm& t);
int    minuteOfDay();                       // -1 if time unknown
int    parseHM(const char* s);              // "07:30" -> 450, -1 if bad
bool   inWindow(int m, int from, int to);   // window may wrap midnight
void   fmtTime(char* out, size_t n, int h, int m, bool h24, bool suffix = true);
extern const char* const WDAY3[7];
extern const char* const MON3[12];

// ---------- command bus ----------
enum CmdType : uint8_t {
  CMD_GESTURE = 1,   // a = gesture id (virtual touch from dashboard)
  CMD_ACTION,        // s = "next" | "prev" | "home" | "pin" | "screen" | "goto" (a = index)
  CMD_MESSAGE,       // s = text, s2 = from, a = seconds
  CMD_NOTIFY,        // s = text, s2 = title, a = kind (AlertKind)
  CMD_EMO,           // s = mood name
  CMD_SOUND,         // s = sound spec (test)
  CMD_APPLY_SETTINGS,// pending JSON waiting in the mailbox
  CMD_APPLY_CARDS,
  CMD_RESTORE,
  CMD_REBOOT,
  CMD_FACTORY,
  CMD_FEED_EVENT,    // alert from the hub: s = title, s2 = text, a = kind, b = mood hint
  CMD_HUB_CMD,       // s = command from hub ("next", "emo:happy" ...)
};

struct Cmd {
  uint8_t type;
  int16_t a, b;
  char s[201];
  char s2[33];
};

void busBegin();
bool busPost(uint8_t type, const char* s = nullptr, const char* s2 = nullptr, int16_t a = 0, int16_t b = 0);
bool busTake(Cmd& c);                       // UI loop only

// Mailbox for big JSON bodies (settings/cards/restore). One slot per kind;
// a newer post replaces an older unapplied one.
enum { MB_SETTINGS = 0, MB_CARDS = 1, MB_RESTORE = 2, MB_COUNT };
void mailPut(int slot, const char* data, size_t len);
bool mailTake(int slot, String& out);

// ---------- misc helpers ----------
void   safeCopy(char* dst, const char* src, size_t n);
uint32_t uptimeS();
size_t heapLargest();
bool   saveFileAtomic(const char* path, const char* data, size_t len);
String readFile(const char* path);
void   ledSet(bool on);

// Crash safety: three crashes in a row within a minute -> safe mode
// (hub and audio stay off so you can still reach the dashboard and fix things).
extern bool gSafeMode;
const char* resetReasonStr();
void sysBegin();
void sysLoop();
