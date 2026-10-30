#!/usr/bin/env node
'use strict';
// EmoDesk Hub - does all the internet work for one or more EmoDesks.
//
//   node server.js            (Node 18+, no npm install needed)
//
// Environment:
//   PORT            default 8787
//   HUB_TOKEN       optional shared secret; devices/dashboard send X-Hub-Token
//   TELEGRAM_TOKEN  optional Telegram bot (see src/telegram.js)
const http = require('http');
const { URL } = require('url');
const feed = require('./src/feed');
const football = require('./src/football');
const cricket = require('./src/cricket');
const weather = require('./src/weather');
const news = require('./src/news');
const upstream = require('./src/http');
const telegram = require('./src/telegram');
const { clip } = require('./src/util');

const PORT = +process.env.PORT || 8787;
const TOKEN = process.env.HUB_TOKEN || '';
const VERSION = '2.0.0';
const started = Date.now();

function send(res, code, body, headers = {}) {
  const data = body === undefined ? '' : typeof body === 'string' ? body : JSON.stringify(body);
  res.writeHead(code, {
    'Content-Type': typeof body === 'string' && body.startsWith('<') ? 'text/html; charset=utf-8' : 'application/json',
    'Content-Length': Buffer.byteLength(data),
    'Cache-Control': 'no-store',
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Headers': 'Content-Type, X-Hub-Token',
    'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
    ...headers,
  });
  res.end(data);
}

function readBody(req, limit = 32768) {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks = [];
    req.on('data', (c) => {
      size += c.length;
      if (size > limit) { reject(new Error('body too large')); req.destroy(); return; }
      chunks.push(c);
    });
    req.on('end', () => {
      const raw = Buffer.concat(chunks).toString('utf8');
      if (!raw) return resolve({});
      try { resolve(JSON.parse(raw)); } catch (e) { reject(new Error('invalid JSON')); }
    });
    req.on('error', reject);
  });
}

function authed(req, url) {
  if (!TOKEN) return true;
  return req.headers['x-hub-token'] === TOKEN || url.searchParams.get('k') === TOKEN;
}

function statusText() {
  const lines = [`EmoDesk Hub ${VERSION}, up ${Math.round((Date.now() - started) / 60000)} min`];
  for (const s of feed.subs.values()) {
    const ago = Math.round((Date.now() - s.lastSeen) / 1000);
    lines.push(`- ${s.cfg.name}: seen ${ago}s ago, feed v${s.v}${s.hot ? ', live match' : ''}`);
  }
  if (feed.subs.size === 0) lines.push('No devices connected yet.');
  return lines.join('\n');
}

function homePage() {
  const rows = [...feed.subs.values()].map((s) => `<tr><td>${s.cfg.name}</td><td>${Math.round((Date.now() - s.lastSeen) / 1000)} s ago</td><td>v${s.v}</td><td>${s.hot ? 'Live match' : ''}</td><td>${(s.feed && s.feed.err || []).join(', ')}</td></tr>`).join('');
  return `<!doctype html><meta name=viewport content="width=device-width,initial-scale=1"><title>EmoDesk Hub</title>
<style>body{font:15px/1.5 system-ui,sans-serif;max-width:720px;margin:40px auto;padding:0 16px;color:#1c2433;background:#eef1f4}
table{border-collapse:collapse;width:100%;background:#fff;border-radius:10px;overflow:hidden}td,th{padding:8px 12px;text-align:left;border-bottom:1px solid #e3e7ec}th{font-weight:600;color:#5e6b80}</style>
<h1>EmoDesk Hub ${VERSION}</h1><p>Running for ${Math.round((Date.now() - started) / 60000)} minutes. ${upstream.stats.requests} upstream requests, ${upstream.stats.failures} failed.</p>
<p>Point your EmoDesk at this address in <b>Settings &gt; Hub</b>.</p>
<table><tr><th>Device</th><th>Last seen</th><th>Feed</th><th>Status</th><th>Sources failing</th></tr>${rows || '<tr><td colspan=5>No devices yet</td></tr>'}</table>`;
}

async function route(req, res) {
  const url = new URL(req.url, 'http://hub');
  const p = url.pathname.replace(/\/+$/, '') || '/';
  if (req.method === 'OPTIONS') return send(res, 204);
  if (p === '/') return send(res, 200, homePage());
  if (p === '/health') return send(res, 200, { ok: true, version: VERSION, uptime: Math.round((Date.now() - started) / 1000), devices: feed.subs.size, upstream: upstream.stats, auth: !!TOKEN });
  if (!authed(req, url)) return send(res, 401, { error: 'bad_token' });

  // Device: register what it wants. Returns the subscription id to poll.
  if (p === '/v1/sub' && req.method === 'POST') {
    const s = feed.register(await readBody(req));
    if (!s.feed) await Promise.race([feed.build(s), new Promise((r) => setTimeout(r, 9000))]);
    return send(res, 200, { id: s.id, v: s.v });
  }

  // Device: long-poll the feed. 304 = nothing new within `wait` seconds.
  if (p === '/v1/feed' && req.method === 'GET') {
    const s = feed.subs.get(url.searchParams.get('sub') || '');
    if (!s) return send(res, 404, { error: 'unknown_sub' });
    s.lastSeen = Date.now();
    if (!s.feed) await Promise.race([feed.build(s), new Promise((r) => setTimeout(r, 9000))]);
    const since = +url.searchParams.get('v') || 0;
    const wait = Math.max(0, Math.min(50, +url.searchParams.get('wait') || 0)) * 1000;
    if (s.v === since && s.body && wait) {
      let gone = false;
      req.on('close', () => { gone = true; });
      const changed = await feed.waitFor(s, wait);
      if (gone) return;
      if (!changed) return send(res, 304);
    }
    if (!s.body) return send(res, 503, { error: 'not_ready' });
    s.lastSeen = Date.now();
    return send(res, 200, s.body);
  }

  // Anyone with the token: show a message on every EmoDesk (or one by name).
  if (p === '/v1/push' && req.method === 'POST') {
    const b = await readBody(req);
    const text = clip(b.text || b.message || '', 200);
    if (!text && !b.cmd) return send(res, 400, { error: 'text required' });
    const ev = b.cmd ? { k: 'cmd', c: clip(b.cmd, 24), ti: '', tx: '' } : { k: 'msg', ti: clip(b.from || b.title || 'Hub', 16), tx: text };
    return send(res, 200, { ok: true, delivered: feed.broadcast(ev, b.to) });
  }

  // Dashboard helpers: pickers instead of free-text team names.
  if (p === '/v1/meta/leagues') return send(res, 200, football.LEAGUES.map(([id, name, abbr]) => ({ id, name, abbr })));
  if (p === '/v1/meta/news') return send(res, 200, Object.keys(news.SOURCES));
  if (p === '/v1/meta/teams') {
    const sport = url.searchParams.get('sport');
    if (sport === 'cricket') return send(res, 200, await cricket.teamList());
    const lg = url.searchParams.get('lg') || 'eng.1';
    return send(res, 200, await football.teams(lg).catch(() => []));
  }
  if (p === '/v1/meta/geocode') {
    const q = (url.searchParams.get('q') || '').trim();
    if (q.length < 2) return send(res, 200, []);
    return send(res, 200, await weather.geocode(q).catch(() => []));
  }
  if (p === '/v1/preview') {
    // Build a feed for an ad-hoc subscription (dashboard "test" button).
    const b = req.method === 'POST' ? await readBody(req) : {};
    const s = feed.register(b);
    await feed.build(s);
    return send(res, 200, s.body);
  }
  return send(res, 404, { error: 'not_found' });
}

const server = http.createServer((req, res) => {
  route(req, res).catch((e) => {
    console.error('[http]', req.url, e.message);
    if (!res.headersSent) send(res, 500, { error: e.message });
  });
});
// Long polls + keep-alive: ESP32 reuses one connection for many polls, which
// avoids a TLS handshake (~1-3 s and 30 KB RAM on the device) every time.
server.keepAliveTimeout = 65000;
server.headersTimeout = 70000;
server.requestTimeout = 0;

if (require.main === module) {
  server.listen(PORT, () => console.log(`EmoDesk Hub ${VERSION} on http://0.0.0.0:${PORT}${TOKEN ? ' (token required)' : ''}`));
  setInterval(() => feed.tick(), 2500);
  setInterval(() => upstream.sweep(), 3600000);
  telegram.start({ broadcast: (ev) => feed.broadcast(ev), status: statusText });
  const stop = () => { server.close(); process.exit(0); };
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
}

module.exports = { server, route };
