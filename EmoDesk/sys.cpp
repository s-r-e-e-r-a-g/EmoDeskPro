#include "sys.h"
#include <LittleFS.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <time.h>

const char* const WDAY3[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const MON3[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

bool gSafeMode = false;

// ------------------------------------------------------------------ log
static char logBuf[4096];
static size_t logHead = 0;     // next write position
static bool logWrapped = false;
static SemaphoreHandle_t logMx = nullptr;

void logf(const char* fmt, ...) {
  char line[192];
  uint32_t s = millis() / 1000;
  int n = snprintf(line, sizeof(line), "[%02lu:%02lu:%02lu] ", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
  va_end(ap);
  size_t len = strlen(line);
  line[len++] = '\n';
  line[len] = 0;
  Serial.print(line);
  if (!logMx) return;
  xSemaphoreTake(logMx, portMAX_DELAY);
  for (size_t i = 0; i < len; i++) {
    logBuf[logHead++] = line[i];
    if (logHead >= sizeof(logBuf)) { logHead = 0; logWrapped = true; }
  }
  xSemaphoreGive(logMx);
}

size_t logCopy(char* out, size_t n) {
  if (!n) return 0;
  xSemaphoreTake(logMx, portMAX_DELAY);
  size_t k = 0;
  if (logWrapped) {
    // skip the partial first line
    size_t i = logHead;
    while (i < sizeof(logBuf) && logBuf[i] != '\n') i++;
    for (i++; i < sizeof(logBuf) && k < n - 1; i++) out[k++] = logBuf[i];
  }
  for (size_t i = 0; i < logHead && k < n - 1; i++) out[k++] = logBuf[i];
  out[k] = 0;
  xSemaphoreGive(logMx);
  return k;
}

// ------------------------------------------------------------------ time
bool timeValid() { return time(nullptr) > 1700000000; }

bool localNow(struct tm& t) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  localtime_r(&now, &t);
  return true;
}

int minuteOfDay() {
  struct tm t;
  if (!localNow(t)) return -1;
  return t.tm_hour * 60 + t.tm_min;
}

int parseHM(const char* s) {
  if (!s) return -1;
  int h, m;
  if (sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

bool inWindow(int m, int from, int to) {
  if (m < 0 || from < 0 || to < 0 || from == to) return false;
  return from < to ? (m >= from && m < to) : (m >= from || m < to);
}

void fmtTime(char* out, size_t n, int h, int m, bool h24, bool suffix) {
  if (h24) { snprintf(out, n, "%02d:%02d", h, m); return; }
  int hh = h % 12;
  if (!hh) hh = 12;
  if (suffix) snprintf(out, n, "%d:%02d%s", hh, m, h < 12 ? "am" : "pm");
  else snprintf(out, n, "%d:%02d", hh, m);
}

// ------------------------------------------------------------------ bus
static QueueHandle_t busQ = nullptr;
static SemaphoreHandle_t mailMx = nullptr;
static char* mail[MB_COUNT] = {nullptr};
static size_t mailLen[MB_COUNT] = {0};

void busBegin() {
  if (!logMx) logMx = xSemaphoreCreateMutex();
  if (!busQ) busQ = xQueueCreate(16, sizeof(Cmd));
  if (!mailMx) mailMx = xSemaphoreCreateMutex();
}

bool busPost(uint8_t type, const char* s, const char* s2, int16_t a, int16_t b) {
  if (!busQ) return false;
  Cmd c;
  memset(&c, 0, sizeof(c));
  c.type = type;
  c.a = a;
  c.b = b;
  if (s) safeCopy(c.s, s, sizeof(c.s));
  if (s2) safeCopy(c.s2, s2, sizeof(c.s2));
  return xQueueSend(busQ, &c, pdMS_TO_TICKS(20)) == pdTRUE;
}

bool busTake(Cmd& c) { return busQ && xQueueReceive(busQ, &c, 0) == pdTRUE; }

void mailPut(int slot, const char* data, size_t len) {
  if (slot < 0 || slot >= MB_COUNT) return;
  char* copy = (char*)malloc(len + 1);
  if (!copy) { logf("[SYS] no memory for %u byte request", (unsigned)len); return; }
  memcpy(copy, data, len);
  copy[len] = 0;
  xSemaphoreTake(mailMx, portMAX_DELAY);
  free(mail[slot]);
  mail[slot] = copy;
  mailLen[slot] = len;
  xSemaphoreGive(mailMx);
}

bool mailTake(int slot, String& out) {
  if (slot < 0 || slot >= MB_COUNT) return false;
  xSemaphoreTake(mailMx, portMAX_DELAY);
  char* p = mail[slot];
  mail[slot] = nullptr;
  xSemaphoreGive(mailMx);
  if (!p) return false;
  out = p;
  free(p);
  return true;
}

// ------------------------------------------------------------------ helpers
void safeCopy(char* dst, const char* src, size_t n) {
  if (!dst || !n) return;
  if (!src) { dst[0] = 0; return; }
  strlcpy(dst, src, n);
}

uint32_t uptimeS() { return (uint32_t)(esp_timer_get_time() / 1000000ULL); }
size_t heapLargest() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }

// Write to .tmp then rename: a power cut mid-save never leaves a torn file.
bool saveFileAtomic(const char* path, const char* data, size_t len) {
  String tmp = String(path) + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  size_t w = f.write((const uint8_t*)data, len);
  f.close();
  if (w != len) { LittleFS.remove(tmp); return false; }
  LittleFS.remove(path);
  return LittleFS.rename(tmp, path);
}

String readFile(const char* path) {
  if (!LittleFS.exists(path)) {
    // recover from a power cut between remove and rename
    String tmp = String(path) + ".tmp";
    if (LittleFS.exists(tmp)) LittleFS.rename(tmp, path);
    else return String();
  }
  File f = LittleFS.open(path, "r");
  if (!f) return String();
  String s = f.readString();
  f.close();
  return s;
}

void ledSet(bool on) {
  digitalWrite(PIN_LED, (on ^ (bool)LED_ACTIVE_LOW) ? HIGH : LOW);
}

// ------------------------------------------------------------------ crash safety
RTC_NOINIT_ATTR static uint32_t rtcMagic;
RTC_NOINIT_ATTR static uint32_t rtcCrashes;
static bool markedGood = false;

const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (power supply too weak)";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    default: return "other";
  }
}

void sysBegin() {
  busBegin();
  pinMode(PIN_LED, OUTPUT);
  ledSet(false);
  esp_reset_reason_t r = esp_reset_reason();
  bool crash = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
  if (rtcMagic != 0xE30D2) { rtcMagic = 0xE30D2; rtcCrashes = 0; }
  rtcCrashes = crash ? rtcCrashes + 1 : 0;
  gSafeMode = rtcCrashes >= 3;
  if (!LittleFS.begin(true)) logf("[SYS] LittleFS mount failed - formatted");
}

void sysLoop() {
  // After a minute of normal running: clear the crash counter and confirm a
  // freshly flashed OTA image so the bootloader does not roll it back.
  if (!markedGood && millis() > 60000) {
    markedGood = true;
    rtcCrashes = 0;
    esp_ota_mark_app_valid_cancel_rollback();
  }
}
