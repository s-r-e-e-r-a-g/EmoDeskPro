# EmoDesk Hub

Does the internet work for one or more EmoDesks: sports, weather, air quality, markets, news, calendars and Telegram. Node.js 18+, no `npm install`.

```bash
node server.js          # listens on :8787
npm test                # unit + end-to-end tests with recorded-shape fixtures
```

Open `http://<hub>:8787/` to see connected devices and which sources are failing.

## Environment

| Variable | Default | |
|---|---|---|
| `PORT` | `8787` | |
| `HUB_TOKEN` | none | Shared secret. Devices and the dashboard send it as `X-Hub-Token` |
| `TELEGRAM_TOKEN` | none | Enables the bot. Send it `/start` to pair, then any text appears on Emo. `/emo happy`, `/next`, `/status` |
| `TELEGRAM_CHATS` | none | Comma-separated chat ids allowed to use the bot (otherwise the first `/start` pairs) |
| `HUB_DATA` | `./hub-data.json` | Where the Telegram pairing is stored |

## How it works

1. The device `POST`s its subscription to `/v1/sub` (location, followed team ids, leagues, tickers, news source, calendar URL). The hub answers with a short id.
2. The device long-polls `GET /v1/feed?sub=<id>&v=<version>&wait=20`. The hub answers immediately when the feed changes, or `304` after 20 s. One kept-alive connection serves every poll, so there is no TLS handshake per request on the ESP32.
3. A background loop rebuilds each feed every 30 s, or every 5 s while a followed match is live. Every upstream call goes through a cache with request coalescing and stale-on-error: if ESPN or CoinGecko hiccups, the device keeps the last good data.
4. Goals, disallowed goals, kick-offs, full time, wickets, match starts and results are found by comparing snapshots, and sent as events with sequence numbers so each one shows exactly once.

## Sources (no API keys)

| Data | Source | Refresh |
|---|---|---|
| Football scores, scorers, red cards | ESPN site API scoreboard (today + a -3..+10 day window) | 15 s live, 5 min otherwise |
| League tables | ESPN standings | 5 min live, 30 min otherwise |
| Cricket | ESPN cricket header (all active series), enriched from the series scoreboard while live | 20 s live, 3 min otherwise |
| Weather, air quality, city search | Open-Meteo | 10 / 30 min |
| Crypto | CoinGecko markets (with 2-day sparkline) | 2 min |
| Stocks | Yahoo Finance chart API | 2 min |
| Currency | open.er-api.com | 1 h |
| News | BBC, Guardian, The Hindu, ESPNcricinfo, Hacker News, The Verge, Google News search, or any RSS | 15 min |
| Calendar | Any iCal URL; repeating events (RRULE, EXDATE, overrides) expanded | 15 min |

ESPN and Yahoo endpoints are public but unofficial and may change. Their parsers are small and isolated in `src/football.js`, `src/cricket.js` and `src/markets.js`.

## API

| | |
|---|---|
| `POST /v1/sub` | register a subscription, returns `{id, v}` |
| `GET /v1/feed?sub=&v=&wait=` | the device feed (long-poll) |
| `POST /v1/push` `{text, from?, to?}` or `{cmd:"next"}` | message or command to every device (or the one named `to`) |
| `GET /v1/meta/leagues`, `/v1/meta/teams?sport=football&lg=eng.1`, `/v1/meta/teams?sport=cricket`, `/v1/meta/geocode?q=` | pickers for the dashboard |
| `POST /v1/preview` | build a feed for a subscription right away (dashboard "Live data") |
| `GET /health` | status |
