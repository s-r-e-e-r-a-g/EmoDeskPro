#include "audio.h"
#include "sys.h"
#include "settings.h"
#include <ESP_I2S.h>
#include <math.h>
#include <freertos/queue.h>

struct Note { uint16_t f; uint16_t ms; };   // f = 0 rest

#define N(f, ms) {f, ms}
static const Note S_TAP[] = {N(1800, 18)};
static const Note S_BOOT[] = {N(523, 70), N(659, 70), N(784, 70), N(1047, 140)};
static const Note S_ALERT[] = {N(988, 90), N(0, 40), N(1319, 160)};
static const Note S_MSG[] = {N(1175, 70), N(1568, 120)};
static const Note S_GOAL[] = {N(523, 90), N(659, 90), N(784, 90), N(1047, 200), N(0, 60), N(784, 90), N(1047, 320)};
static const Note S_WICKET[] = {N(880, 110), N(0, 30), N(880, 110), N(0, 30), N(659, 260)};
static const Note S_SAD[] = {N(659, 160), N(587, 160), N(523, 320)};
static const Note S_HAPPY[] = {N(784, 70), N(988, 70), N(1175, 140)};
static const Note S_ALARM[] = {N(1319, 120), N(0, 60), N(1319, 120), N(0, 60), N(1319, 120), N(0, 500)};
static const Note S_TIMER[] = {N(1047, 150), N(0, 80), N(1047, 150), N(0, 80), N(1568, 300)};
static const Note S_CHIME[] = {N(1319, 220), N(1047, 220), N(1175, 220), N(784, 440)};
static const Note S_POKE[] = {N(1400, 30), N(1900, 40)};

struct Sfx { const char* name; const Note* n; uint8_t len; };
#define SFX(name, arr) {name, arr, (uint8_t)(sizeof(arr) / sizeof(arr[0]))}
static const Sfx SFXS[] = {
  SFX("tap", S_TAP), SFX("boot", S_BOOT), SFX("alert", S_ALERT), SFX("msg", S_MSG), SFX("goal", S_GOAL),
  SFX("wicket", S_WICKET), SFX("sad", S_SAD), SFX("happy", S_HAPPY), SFX("alarm", S_ALARM), SFX("timer", S_TIMER),
  SFX("chime", S_CHIME), SFX("poke", S_POKE),
};
static const int NSFX = sizeof(SFXS) / sizeof(SFXS[0]);
static const char* NAMES[NSFX];

static I2SClass i2s;
static QueueHandle_t q = nullptr;
static TaskHandle_t task = nullptr;
static volatile bool stopReq = false, busy = false;
static volatile uint8_t vol = 40;
static bool ready = false;

struct Req { int8_t idx; bool loop; };

static void ampOn(bool on) {
#if PIN_AMP_SD >= 0
  digitalWrite(PIN_AMP_SD, on ? HIGH : LOW);
#endif
}

// The C3 has no FPU, so the synth is integer only: a 256-step sine table and
// a 32-bit phase accumulator (plus a quieter octave for a warmer tone).
static int16_t sineT[256];

static void playNote(const Note& n) {
  const int rate = AUDIO_RATE;
  int total = (int)((uint32_t)n.ms * rate / 1000);
  int16_t buf[128];
  uint32_t phase = 0, step = n.f ? (uint32_t)(((uint64_t)n.f << 32) / rate) : 0;
  int32_t amp = (int32_t)vol * vol * 12 / 10;                // 0..12000, perceptual curve
  int attack = rate / 200, release = rate / 60;              // 5 ms / 16 ms
  for (int i = 0; i < total && !stopReq;) {
    int k = 0;
    for (; k < 128 && i < total; k++, i++) {
      int32_t env = 256;
      if (i < attack) env = i * 256 / attack;
      else if (total - i < release) env = (total - i) * 256 / release;
      int32_t s = 0;
      if (n.f) {
        s = sineT[phase >> 24] + (sineT[(phase << 1) >> 24] >> 2);   // fundamental + 1/4 octave
        phase += step;
      }
      buf[k] = (int16_t)((s * amp / 32767) * env / 256 * 4 / 5);
    }
    i2s.write((uint8_t*)buf, k * sizeof(int16_t));
  }
}

static void audioTask(void*) {
  Req r;
  for (;;) {
    if (xQueueReceive(q, &r, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
    stopReq = false;
    busy = true;
    ampOn(true);
    vTaskDelay(pdMS_TO_TICKS(5));
    const Sfx& s = SFXS[r.idx];
    uint32_t loopUntil = millis() + 5UL * 60000;   // an alarm stops by itself after 5 min
    do {
      for (int i = 0; i < s.len && !stopReq; i++) playNote(s.n[i]);
    } while (r.loop && !stopReq && millis() < loopUntil && uxQueueMessagesWaiting(q) == 0);
    int16_t z[64] = {0};
    for (int i = 0; i < 4; i++) i2s.write((uint8_t*)z, sizeof(z));   // flush DMA with silence
    vTaskDelay(pdMS_TO_TICKS(30));
    if (!uxQueueMessagesWaiting(q)) ampOn(false);
    busy = false;
  }
}

void audioBegin() {
  for (int i = 0; i < NSFX; i++) NAMES[i] = SFXS[i].name;
  for (int i = 0; i < 256; i++) sineT[i] = (int16_t)(sinf(i * 2 * M_PI / 256) * 32767);
#if PIN_AMP_SD >= 0
  pinMode(PIN_AMP_SD, OUTPUT);
  ampOn(false);
#endif
  audioApply();
}

void audioApply() {
  vol = S.volume;
  if (!S.sound || ready || gSafeMode) return;
  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DIN);
  if (!i2s.begin(I2S_MODE_STD, AUDIO_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    logf("[AUDIO] I2S start failed");
    return;
  }
  q = xQueueCreate(4, sizeof(Req));
  xTaskCreate(audioTask, "audio", AUDIO_STACK, nullptr, AUDIO_PRIO, &task);
  ready = true;
  logf("[AUDIO] speaker ready");
}

bool audioPlay(const char* name, uint8_t ev, bool loop) {
  if (!ready || !S.sound || !name) return false;
  if (ev < 8 && !(S.sndMask & (1 << ev))) return false;
  if (ev != SE_ALARM && ev != SE_TIMER) {
    int m = minuteOfDay();
    if (m >= 0 && inWindow(m, parseHM(S.quietFrom), parseHM(S.quietTo))) return false;
  }
  for (int i = 0; i < NSFX; i++) {
    if (strcmp(SFXS[i].name, name)) continue;
    if (loop || ev == SE_ALARM) { stopReq = true; xQueueReset(q); }
    Req r = {(int8_t)i, loop};
    return xQueueSend(q, &r, 0) == pdTRUE;
  }
  return false;
}

void audioStop() {
  stopReq = true;
  if (q) xQueueReset(q);
}

bool audioBusy() { return busy; }

const char* const* audioNames(int& n) {
  n = NSFX;
  return NAMES;
}
