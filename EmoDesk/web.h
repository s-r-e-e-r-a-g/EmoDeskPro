#pragma once
#include <Arduino.h>

// Async web server: the dashboard, a JSON API and a WebSocket that streams the
// OLED frames and live status. Runs in the AsyncTCP task, so serving the
// dashboard never freezes the screen or touch (the old synchronous server
// blocked the main loop for every request).
void webBegin();
void webLoop();                     // UI loop: push frames/status to dashboard clients
void webStatusJson(String& out);
