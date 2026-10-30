'use strict';
// Crypto (CoinGecko), stocks (Yahoo chart API) and currency (open.er-api.com).
const { getJson, cached } = require('./http');
const { clip, num, round1, spark, fmtPrice } = require('./util');

async function crypto(ids, vs = 'usd') {
  ids = ids.filter((x) => /^[a-z0-9-]{1,40}$/.test(x)).slice(0, 8);
  if (!ids.length) return [];
  vs = /^[a-z]{3,4}$/.test(vs) ? vs : 'usd';
  return cached(`mk:cg:${vs}:${ids.join(',')}`, 2 * 60000, async () => {
    const j = await getJson(`https://api.coingecko.com/api/v3/coins/markets?vs_currency=${vs}&ids=${ids.join(',')}&sparkline=true&price_change_percentage=24h`);
    const by = new Map(j.map((c) => [c.id, c]));
    return ids.filter((id) => by.has(id)).map((id) => {
      const c = by.get(id);
      const pts = ((c.sparkline_in_7d || {}).price || []).slice(-48); // ~last 2 days
      return { s: clip(String(c.symbol || id).toUpperCase(), 6), p: fmtPrice(c.current_price), c: round1(c.price_change_percentage_24h), sp: spark(pts), k: 'c' };
    });
  });
}

async function stock(sym) {
  if (!/^[A-Za-z0-9.^=\-]{1,15}$/.test(sym)) return null;
  return cached(`mk:yf:${sym}`, 2 * 60000, async () => {
    const j = await getJson(`https://query1.finance.yahoo.com/v8/finance/chart/${encodeURIComponent(sym)}?range=1d&interval=15m`);
    const r = ((j.chart || {}).result || [])[0];
    if (!r) throw new Error(`no data for ${sym}`);
    const m = r.meta || {};
    const closes = ((((r.indicators || {}).quote || [])[0] || {}).close || []).filter((x) => x !== null);
    const price = num(m.regularMarketPrice, closes[closes.length - 1]);
    const prev = num(m.chartPreviousClose || m.previousClose, closes[0]);
    const label = sym.replace(/\.(NS|BO|L|TO|AX|DE|PA)$/i, '').replace(/^\^/, '');
    return { s: label.toUpperCase().slice(0, 9), p: fmtPrice(price), c: prev ? round1(((price - prev) / prev) * 100) : 0, sp: spark(closes), k: 's' };
  });
}

async function fx(pairs) {
  const out = [];
  for (const pair of pairs.slice(0, 6)) {
    const [b, q] = String(pair).toUpperCase().split(/[:/]/);
    if (!/^[A-Z]{3}$/.test(b || '') || !/^[A-Z]{3}$/.test(q || '')) continue;
    try {
      const rates = await cached(`mk:fx:${b}`, 60 * 60000, async () => (await getJson(`https://open.er-api.com/v6/latest/${b}`)).rates || {});
      if (rates[q]) out.push({ s: `${b}/${q}`, p: fmtPrice(rates[q]), c: 0, sp: [], k: 'f' });
    } catch (_) { /* skip pair */ }
  }
  return out;
}

async function forSub(mk) {
  const parts = await Promise.all([
    crypto(mk.cr || [], mk.vs || 'usd').catch(() => []),
    Promise.all((mk.st || []).slice(0, 8).map((s) => stock(s).catch(() => null))),
    fx(mk.fx || []).catch(() => []),
  ]);
  return [...parts[0], ...parts[1].filter(Boolean), ...parts[2]];
}

module.exports = { crypto, stock, fx, forSub };
