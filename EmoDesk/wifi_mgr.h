#pragma once
#include <Arduino.h>

// Non-blocking Wi-Fi manager.
//  * One connection attempt at a time, 15 s timeout, then the next saved
//    network, with backoff (5 s -> 60 s). Auto-reconnect inside the Wi-Fi
//    driver is off, so the two never fight each other (the old firmware called
//    WiFi.begin() every 60 s on top of auto-reconnect, aborting attempts that
//    were about to succeed).
//  * mDNS restarts on every new IP, so emo.local keeps working after a drop.
//  * The setup hotspot opens after 45 s without a connection (immediately if
//    no network is saved) and pauses retries while a phone is connected to it.
enum WifiState : uint8_t { WF_NO_CREDS = 0, WF_CONNECTING, WF_CONNECTED, WF_WAITING };

void wifiBegin();
void wifiLoop();                 // UI loop, cheap
bool wifiOnline();
bool wifiApActive();
WifiState wifiState();
const char* wifiSsid();
String wifiIp();
int  wifiRssi();
uint32_t wifiConnectedForS();
void wifiReconnect();            // settings changed: start over
void wifiScanStart();
int  wifiScanResults(String& json);   // -1 running, -2 not started, >=0 count
const char* wifiLastReason();
