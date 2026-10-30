'use strict';
// Upstream fetching with a timeout, a shared cache, request coalescing and
// stale-on-error. Every source goes through cached() so a busy upstream or a
// network blip never empties a card on the device: the last good value stays.
const { UA } = require('./util');

const cache = new Map(); // key -> { ts, ttl, data, err, inflight }
const stats = { requests: 0, failures: 0, lastError: '' };

async function getRaw(url, { timeout = 8000, headers = {}, accept = 'application/json' } = {}) {
  stats.requests++;
  const res = await fetch(url, {
    headers: { 'User-Agent': UA, Accept: accept, ...headers },
    signal: AbortSignal.timeout(timeout),
    redirect: 'follow',
  });
  if (!res.ok) {
    const e = new Error(`HTTP ${res.status} ${new URL(url).host}`);
    e.status = res.status;
    throw e;
  }
  return res;
}

async function getJson(url, opts) {
  const res = await getRaw(url, opts);
  return res.json();
}

async function getText(url, opts) {
  const res = await getRaw(url, { accept: '*/*', ...opts });
  return res.text();
}

// Returns cached data when younger than ttl ms, otherwise refreshes. Concurrent
// callers share one upstream request. On failure the stale value is returned
// (if any) and the entry retries after a short backoff instead of hammering.
async function cached(key, ttl, fn) {
  const now = Date.now();
  let e = cache.get(key);
  if (e && e.data !== undefined && now - e.ts < ttl) return e.data;
  if (e && e.retryAt && now < e.retryAt) {
    if (e.data !== undefined) return e.data;
    throw e.err || new Error('backing off');
  }
  if (e && e.inflight) return e.inflight;
  if (!e) { e = {}; cache.set(key, e); }
  e.inflight = (async () => {
    try {
      const data = await fn();
      e.data = data; e.ts = Date.now(); e.err = null; e.fails = 0; e.retryAt = 0;
      return data;
    } catch (err) {
      stats.failures++;
      stats.lastError = `${key}: ${err.message}`;
      e.err = err;
      e.fails = (e.fails || 0) + 1;
      e.retryAt = Date.now() + Math.min(300000, 5000 * 2 ** Math.min(e.fails, 6));
      if (e.data !== undefined) return e.data;
      throw err;
    } finally {
      e.inflight = null;
    }
  })();
  return e.inflight;
}

function peek(key) {
  const e = cache.get(key);
  return e ? e.data : undefined;
}

// Drop entries nobody asked for in a while so the hub never grows without bound.
function sweep(maxAgeMs = 6 * 3600000) {
  const now = Date.now();
  for (const [k, e] of cache) if (!e.inflight && now - (e.ts || 0) > maxAgeMs) cache.delete(k);
}

module.exports = { getJson, getText, cached, peek, sweep, stats, cache };
