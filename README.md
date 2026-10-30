# EmoDesk 2

An Emo-style desk companion for the **ESP32-C3 Super Mini** with a **128x64 SSD1306 OLED** and a **TTP223 touch pad** (optional **MAX98357A** speaker). Same hardware and wiring as EmoDesk 1, rebuilt from scratch to fix performance, connectivity, the dashboard, the OLED screens and the sports cards.

```
 ┌──────────────┐  one small JSON feed,     ┌──────────────┐   ESPN, Open-Meteo,
 │   EmoDesk    │  long-poll: answers the   │  EmoDesk Hub │   CoinGecko, Yahoo,
 │  (ESP32-C3)  │◄─ moment a goal goes in ──│  (Node 18+)  │◄─ RSS, iCal, Telegram
 └──────┬───────┘                           └──────────────┘
        │  dashboard + live screen mirror (WebSocket)
   phone / PC
```

The device draws and reacts. The **hub** does the internet work: it polls the sports, market and news sources, matches your teams, spots goals and wickets, and sends Emo a compact feed. Without a hub, Emo still shows the clock, the weather and the timers.

---

## What was wrong in v1, and what v2 does instead

| Problem in v1 | Cause | v2 |
|---|---|---|
| **Sports did not work from a fresh clone** | The cricket/football server (`server/server`) was a broken git submodule with no `.gitmodules`, so the code the firmware called was missing from the repo | The hub (`hub/`) is in the repo, has no dependencies and is covered by tests |
| **Sports delays, missed alerts** | Cricket was only fetched while its card was on screen, so alerts never fired in the background and data was stale on arrival. Every card fetch waited in a queue with a 1.5 s gap and a TLS handshake each | The hub watches every followed match all the time (15-20 s while live) and the device long-polls: the request returns as soon as the score changes. Goals and wickets get ids, so each alert shows exactly once |
| **Wrong matches for your team** | Fuzzy name matching ("India" vs "India Women", `isQualifierMismatch` patches) | Teams are picked from a list in the dashboard and matched by ESPN team id |
| **Low performance, stutter** | One global recursive lock held while drawing, during web requests and by the network task. Settings looked up in JSON on every frame at 30 fps | The UI loop owns the screen; the web and net tasks only post commands. Settings are a plain struct. Each card redraws only as often as it needs to, and only changed 8x8 tiles go over I2C |
| **Screen and touch froze when the dashboard was open** | Synchronous `WebServer` blocked the main loop while sending the 38 KB page, and the screen mirror polled | Async web server in its own task. Dashboard is 17 KB gzipped. Screen frames stream over a WebSocket only when the picture changes |
| **Wi-Fi dropped and did not come back** | `WiFi.begin()` called every 60 s on top of the driver's auto-reconnect, aborting attempts that were about to succeed. mDNS started once, so `emo.local` died after a reconnect | One connection attempt at a time with backoff, the driver's auto-reconnect is off, mDNS restarts on every new IP, and the setup hotspot pauses retries while a phone is connected to it |
| **OLED glitches, cramped screens** | I2C at 800 kHz (twice the SSD1306 rating). Ad-hoc layouts per card | 400 kHz. A shared layout system (status bar, type scale, ellipsis, marquee), slide transitions and burn-in shift applied to every screen without per-card code |
| Hard to maintain | 68 card types, 20k lines, scratch patch scripts in the repo | 21 cards, about 5000 lines of firmware |

### What's not in v2

v2 is focused, so some v1 features are not included. These were the parts that cost the most memory and caused the most instability:

- Bluetooth (presence, media remote), MQTT / Home Assistant auto-discovery, the robot voice (TTS), WhatsApp bridge, serial terminal, picture/GIF upload, battery handling, and 47 of the 68 card types (most games, screensavers, habits, to-dos and similar).
- For Home Assistant and scripts, use the device HTTP API or the hub's `POST /v1/push` (below).

---

## 1. Run the hub (optional, needed for sports, markets, news and calendar)

On any always-on machine on your network (PC, Raspberry Pi, NAS, server). Node.js 18 or newer, nothing to install:

```bash
cd hub
node server.js                 # http://<this-machine>:8787
# or with Docker
docker build -t emodesk-hub hub && docker run -d -p 8787:8787 --restart unless-stopped emodesk-hub
```

Optional environment variables: `HUB_TOKEN` (shared secret), `TELEGRAM_TOKEN` (bot from @BotFather), `PORT`. See [hub/README.md](hub/README.md).

Avoid free hosts that put apps to sleep (for example Render's free tier). A sleeping hub takes 30-60 s to wake up, which looks exactly like the old "sports delays".

## 2. Flash the firmware

### PlatformIO (recommended)

```bash
pio run -e esp32c3_supermini -t upload
```

Libraries install automatically and `web/index.html` is packed into the firmware on every build.

### Arduino IDE 2

1. Boards Manager: **esp32 by Espressif 3.x**.
2. Library Manager: **U8g2**, **ArduinoJson** (7.x), **ESPAsyncWebServer** and **AsyncTCP** (both by *ESP32Async*).
3. Open `EmoDesk/EmoDesk.ino`. Board **ESP32C3 Dev Module**, USB CDC On Boot **Enabled**, Flash size **4MB**. The sketch's `partitions.csv` (two 1.75 MB OTA slots) is used automatically.
4. Upload. If the port does not show up, hold **BOOT** while plugging in USB.

After the first USB flash, updates can go over Wi-Fi from **System > Firmware**. A new image that fails to start rolls back to the previous one.

## 3. First start

1. Emo shows a QR code. Scan it, or join the Wi-Fi **EmoDesk-Setup**.
2. The setup page opens (or browse to **http://192.168.4.1**). Pick your network, enter the password, tap **Connect**.
3. Rejoin your normal Wi-Fi and open **http://emo.local** (or scan the QR code on Emo's **System** card).
4. In **Settings**: search your city (sets the time zone too), paste the hub address, pick your teams.

---

## Using Emo

| Gesture | Everywhere | Emo face | Timers, lists, sports |
|---|---|---|---|
| Tap | next card | next card | next card |
| Double-tap | previous card | poke | start/pause, next match/page |
| Triple-tap | home (Emo) | | |
| Hold 0.6 s | pin the card (stops rotation) | pet (hearts) | reset / lap / next preset |
| Hold 2.5 s | screen off (touch to wake) | | |

In the **Dino** game, touching jumps instantly and holding 1.5 s leaves. The **BOOT** button: tap next, double previous, hold screen off, long hold toggles rotation. The dashboard has the same touch pad under the live screen.

### Cards

Emo, Clock, Analog, Weather, Forecast, Air, Cricket, Football, Table, Markets, News, Quote, Agenda, Focus (pomodoro), Timer, Stopwatch, Alarms, PC, Messages, System, Dino. Cards with nothing to show are skipped in rotation, and a card showing a live match of your team stays on screen twice as long.

### Alerts

Goals (with scorer), disallowed goals, kick-off, full time, wickets, match start, results, messages, alarms and timers take over the screen, play a sound if the speaker is installed, and Emo reacts afterwards: excited for your goal, sad when you concede or lose a wicket.

---

## HTTP API

```bash
curl -X POST http://emo.local/api/message -H 'Content-Type: application/json' -d '{"text":"Dinner is ready","from":"Kitchen"}'
curl -X POST http://emo.local/api/notify  -H 'Content-Type: application/json' -d '{"title":"Doorbell","text":"Someone is at the door"}'
curl -X POST http://emo.local/api/emo     -H 'Content-Type: application/json' -d '{"mood":"love"}'
curl -X POST "http://emo.local/api/action?a=next"          # next | prev | home | pin | screen | rotate | goto&i=3
curl http://emo.local/api/state
```

With a dashboard PIN, add the header `X-Emo-Pin: <pin>`. From anywhere the hub is reachable: `curl -X POST http://hub:8787/v1/push -d '{"text":"Build passed"}'`.

**PC companion:** `pip install psutil` then `python3 tools/pc_companion.py --host emo.local`. It sends CPU, RAM, GPU, temperatures and now playing, and Emo's screen turns off while your PC is locked.

---

## Project layout

```
EmoDesk/            firmware (Arduino sketch folder, also the PlatformIO src_dir)
  EmoDesk.ino       setup/loop, gestures, command handling, screen power
  sys.*             log, time, command bus, crash counter, OTA confirmation
  settings.*        settings struct <-> JSON, debounced atomic saves, hub subscription
  wifi_mgr.*        Wi-Fi state machine, setup hotspot, captive DNS, mDNS
  net.cpp           hub long-poll client, feed parsing, direct weather fallback
  gfx.*             U8g2, compositor (slides, burn-in shift), tile-diff flush, drawing kit
  emo.*             the face: parametric eyes that morph between moods
  cards*.cpp        card engine and the 21 cards
  alerts.*          full-screen alerts
  audio.*           integer synth for the speaker
  web.*             async server, JSON API, WebSocket mirror, OTA
web/index.html      dashboard source (packed into EmoDesk/web_ui.h by tools/build_web.py)
hub/                the hub (Node 18+, no dependencies) with tests: cd hub && npm test
tools/              build_web.py, pc_companion.py, preview/ (renders every OLED screen to PNG on a PC)
```

Edited the dashboard? PlatformIO repacks it automatically; with the Arduino IDE run `python3 tools/build_web.py` first.

To review OLED layouts without hardware: `tools/preview/run.sh` (needs g++, Python Pillow and the U8g2/ArduinoJson sources; see the script) writes a PNG of every card, mood and alert to `tools/preview/out/`.

## Troubleshooting

- **Hub shows "unreachable" on the System card:** open `http://<hub>:8787/` in a browser on the same network. Use the machine's IP, not `localhost`.
- **Wrong or no matches:** check **Live data** in the dashboard. It shows exactly what the hub sends for your settings.
- **Wi-Fi keeps failing:** the System card and the dashboard health panel show the reason (wrong password, network not found, signal lost). Try a higher transmit power if the router is far away. Use 2.4 GHz.
- **"Safe mode":** Emo crashed three times in a row. The hub client and the speaker stay off until the next restart; the log in **System** shows what happened.
- **Black screen:** check SDA/SCL (GPIO 4/5) and that the OLED is at I2C address 0x3C.
