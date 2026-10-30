#pragma once
// =====================================================================
//  EmoDesk 2 - desktop companion for the ESP32-C3 Super Mini
//  SSD1306 128x64 I2C OLED + TTP223 touch (+ optional MAX98357A speaker)
// =====================================================================

#define FW_VERSION      "2.0.0"
#define DEFAULT_HOST    "emo"          // dashboard at http://emo.local
#define SETUP_AP_SSID   "EmoDesk-Setup"

// ---------------- Pins (see WIRING.md) ----------------
#define PIN_SDA         4
#define PIN_SCL         5
#define PIN_TOUCH       3              // TTP223 OUT, active HIGH
#define PIN_BOOT        9              // on-board BOOT button, active LOW
#define PIN_LED         8              // on-board blue LED
#define LED_ACTIVE_LOW  1
#define PIN_I2S_BCLK    6              // MAX98357A BCLK
#define PIN_I2S_LRC     7              // MAX98357A LRC / WS
#define PIN_I2S_DIN     10             // MAX98357A DIN
#define PIN_AMP_SD      1              // MAX98357A SD (amp on/off), -1 if not wired

// 400 kHz is the SSD1306 rating. Faster clocks were the cause of random
// garbage lines on longer wires. Change in Settings > Display if you like.
#define OLED_I2C_HZ_DEFAULT 400000

// ---------------- Limits ----------------
#define MAX_CARDS       24
#define MAX_WIFI        3
#define MAX_ALARMS      8
#define AUDIO_RATE      16000

// ---------------- Tasks ----------------
#define NET_STACK       (10 * 1024)    // TLS (https hub) needs ~7 KB peak
#define NET_PRIO        1
#define AUDIO_STACK     3072
#define AUDIO_PRIO      3
