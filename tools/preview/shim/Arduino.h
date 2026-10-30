// Host shim: just enough Arduino to render EmoDesk screens on a PC.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <string>
#include <algorithm>
#include <strings.h>

extern uint32_t g_millis;
inline uint32_t millis() { return g_millis; }
inline void delay(uint32_t) {}
uint32_t esp_random();
#define PROGMEM
#define HIGH 1
#define LOW 0
#define RTC_NOINIT_ATTR
using std::min;
using std::max;
template <class T, class L, class H> T constrain(T x, L lo, H hi) { return x < (T)lo ? (T)lo : x > (T)hi ? (T)hi : x; }

class String : public std::string {
 public:
  String() {}
  String(const char* s) : std::string(s ? s : "") {}
  String(const std::string& s) : std::string(s) {}
  String(int v) : std::string(std::to_string(v)) {}
  unsigned length() const { return (unsigned)size(); }
  int indexOf(char c, int from = 0) const { auto p = find(c, from); return p == npos ? -1 : (int)p; }
  String substring(int a, int b) const { return String(substr(a, b - a)); }
  String substring(int a) const { return String(substr(a)); }
  void trim() { size_t a = find_first_not_of(" \t\r\n"), b = find_last_not_of(" \t\r\n"); *this = a == npos ? String("") : String(substr(a, b - a + 1)); }
  bool startsWith(const char* p) const { return rfind(p, 0) == 0; }
  bool endsWith(const char* p) const { size_t n = strlen(p); return size() >= n && compare(size() - n, n, p) == 0; }
  void remove(unsigned i) { erase(i); }
  size_t write(uint8_t c) { push_back((char)c); return 1; }
  size_t write(const uint8_t* p, size_t n) { append((const char*)p, n); return n; }
};
inline String operator+(const char* a, const String& b) { return String(std::string(a) + b); }

struct EspClass { uint32_t getFreeHeap() { return 151000; } uint32_t getMinFreeHeap() { return 120000; } };
extern EspClass ESP;
