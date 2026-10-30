'use strict';
// Subscriptions, feed building, alert detection and long-poll waiters.
//
// A device POSTs its subscription (what it shows: location, teams, leagues,
// tickers...) once and then long-polls GET /v1/feed. The hub answers as soon
// as anything in that device's feed changes, or with 304 after ~25 s.
// Goals, wickets, kick-offs and results are detected here by diffing
// successive snapshots, each with a stable id, so the device shows every alert
// exactly once, even if it was busy or reconnecting.
const crypto = require('crypto');
const football = require('./football');
const cricket = require('./cricket');
const weather = require('./weather');
const markets = require('./markets');
const news = require('./news');
const { quote } = require('./quote');
const ical = require('./ical');
const { clip, num } = require('./util');

const BOOT = Math.floor(Date.now() / 1000) % 100000; // lets devices notice a hub restart
const subs = new Map();
let seq = 0;

const arr = (v, n, re) => (Array.isArray(v) ? v : typeof v === 'string' && v ? v.split(',') : [])
  .map((x) => (typeof x === 'string' ? x.trim() : x)).filter((x) => x !== '' && x !== null && x !== undefined)
  .filter((x) => !re || typeof x !== 'string' || re.test(x)).slice(0, n);

// Defensive copy of whatever the device sent: only known keys, bounded sizes.
function normalizeSub(raw = {}) {
  const loc = raw.loc || {};
  const fb = raw.fb || {};
  const cr = raw.cr || {};
  const mk = raw.mk || {};
  const al = raw.al || {};
  const team = (t) => (t && typeof t === 'object' ? { id: String(t.id || '').slice(0, 12), lg: String(t.lg || '').slice(0, 24), abbr: String(t.abbr || '').slice(0, 6).toUpperCase() } : { id: String(t).slice(0, 12) });
  const sub = {
    name: clip(raw.name || 'emo', 24),
    loc: Number.isFinite(+loc.lat) && Number.isFinite(+loc.lon) && (+loc.lat || +loc.lon) ? { lat: +(+loc.lat).toFixed(4), lon: +(+loc.lon).toFixed(4) } : null,
    units: raw.units === 'imperial' ? 'imperial' : 'metric',
    tz: typeof raw.tz === 'string' && /^[A-Za-z_\/+\-0-9]{1,40}$/.test(raw.tz) ? raw.tz : 'UTC',
    wx: !!raw.wx, aq: !!raw.aq, qt: !!raw.qt,
    fb: {
      lg: arr(fb.lg, 6, /^[a-z0-9_.]{2,24}$/),
      tm: arr(fb.tm, 8).map(team).filter((t) => t.id),
      tb: arr(fb.tb, 3, /^[a-z0-9_.]{2,24}$/),
    },
    cr: { tm: arr(cr.tm, 8).map(team).filter((t) => t.id || t.abbr), all: !!cr.all },
    mk: { cr: arr(mk.cr, 8, /^[a-z0-9-]{1,40}$/), st: arr(mk.st, 8, /^[A-Za-z0-9.^=\-]{1,15}$/), fx: arr(mk.fx, 6, /^[A-Za-z]{3}[:/][A-Za-z]{3}$/), vs: /^[a-z]{3,4}$/.test(mk.vs || '') ? mk.vs : 'usd' },
    nw: typeof raw.nw === 'string' ? raw.nw.slice(0, 300) : '',
    ic: typeof raw.ic === 'string' && /^(https?|webcal):\/\//i.test(raw.ic) ? raw.ic.slice(0, 500) : '',
    al: { goal: al.goal !== 0 && al.goal !== false, wkt: al.wkt !== 0 && al.wkt !== false, res: al.res !== 0 && al.res !== false, start: al.start !== 0 && al.start !== false, any: !!al.any },
  };
  return sub;
}

function subId(sub) {
  return crypto.createHash('sha1').update(JSON.stringify(sub)).digest('hex').slice(0, 12);
}

function register(raw) {
  const cfg = normalizeSub(raw);
  const id = subId(cfg);
  let s = subs.get(id);
  if (!s) {
    s = { id, cfg, v: 1, feed: null, body: '', content: '', events: [], waiters: new Set(), lastSeen: Date.now(), prev: null, building: null, hot: false };
    subs.set(id, s);
  }
  s.lastSeen = Date.now();
  return s;
}

function pushEvent(s, ev) {
  ev.q = ++seq;
  ev.ts = Math.floor(Date.now() / 1000);
  s.events.push(ev);
  const cutoff = ev.ts - 600;
  s.events = s.events.filter((e) => e.ts >= cutoff).slice(-12);
}

// Broadcast a message or command (Telegram, /v1/push) to every device, or
// only to devices whose name matches `to`.
function broadcast(ev, to) {
  let n = 0;
  for (const s of subs.values()) {
    if (to && s.cfg.name.toLowerCase() !== String(to).toLowerCase()) continue;
    if (Date.now() - s.lastSeen > 15 * 60000) continue;
    pushEvent(s, { ...ev });
    bump(s);
    n++;
  }
  return n;
}

// ---------------------------------------------------------------- football
async function footballFor(cfg) {
  const leagues = [...new Set([...cfg.fb.lg, ...cfg.fb.tm.map((t) => t.lg).filter(Boolean)])].slice(0, 8);
  if (!leagues.length) return [];
  const teamIds = new Set(cfg.fb.tm.map((t) => t.id));
  const all = (await Promise.all(leagues.map((lg) => football.league(lg).catch(() => [])))).flat();
  const now = Date.now() / 1000;
  const mine = (e) => teamIds.has(e.h.id) || teamIds.has(e.a.id);
  const picked = new Map();
  const add = (e) => { if (!picked.has(e.id)) picked.set(e.id, { ...e, mine: mine(e) }); };
  for (const e of all) {
    const followedLeague = cfg.fb.lg.includes(e.lg);
    if (e.state === 'in' && (followedLeague || mine(e))) add(e);
    else if (followedLeague && e.state === 'pre' && e.start - now < 20 * 3600 && e.start - now > -3 * 3600) add(e);
    else if (followedLeague && e.state === 'post' && now - e.start < 14 * 3600) add(e);
  }
  for (const id of teamIds) {
    const games = all.filter((e) => e.h.id === id || e.a.id === id);
    const next = games.filter((e) => e.state === 'pre' && !e.postponed).sort((x, y) => x.start - y.start)[0];
    const last = games.filter((e) => e.state === 'post').sort((x, y) => y.start - x.start)[0];
    if (next) add(next);
    if (last) add(last);
  }
  const rank = (e) => (e.state === 'in' ? 0 : e.state === 'pre' ? 2 : 1) + (e.mine ? 0 : 3);
  return [...picked.values()]
    .sort((x, y) => rank(x) - rank(y) || (x.state === 'post' ? y.start - x.start : x.start - y.start))
    .slice(0, 14);
}

const goalsText = (goals, side) => clip(goals.filter((g) => g.s === side).map((g) => `${g.who} ${g.min}`).join(', '), 48);

function fbOut(e) {
  return {
    id: e.id, lg: football.lgAbbr(e.lg), lid: e.lg, s: e.state, d: e.detail, t: e.start,
    h: { a: e.h.a, n: e.h.n, s: e.h.sc === null ? -1 : e.h.sc, id: e.h.id },
    a: { a: e.a.a, n: e.a.n, s: e.a.sc === null ? -1 : e.a.sc, id: e.a.id },
    gh: goalsText(e.goals, 'h'), ga: goalsText(e.goals, 'a'),
    rh: e.redH, ra: e.redA, m: e.mine ? 1 : 0, v: e.venue,
  };
}

function crOut(m) {
  return {
    id: m.id, ser: m.serName, ti: m.title, f: m.fmt, s: m.state, st: m.status, t: m.start,
    a: m.t.map((x) => ({ a: x.a, n: x.n, sc: x.sc, ov: x.ov, b: x.bat ? 1 : 0, w: x.win ? 1 : 0, id: x.id })),
    crr: m.crr || 0, rrr: m.rrr || 0, nd: m.need || '', m: m.mine ? 1 : 0,
  };
}

// ---------------------------------------------------------------- alerts
function diffFootball(s, prev, cur) {
  const al = s.cfg.al;
  for (const e of cur) {
    const p = prev.get(e.id);
    if (!p) continue;
    const relevant = e.mine || al.any;
    if (!relevant) continue;
    const score = `${e.h.a} ${Math.max(0, e.h.sc || 0)}-${Math.max(0, e.a.sc || 0)} ${e.a.a}`;
    if (al.start && p.state === 'pre' && e.state === 'in') pushEvent(s, { k: 'kick', ti: 'Kick-off', tx: `${e.h.a} v ${e.a.a}`, m: e.mine ? 1 : 0 });
    if (al.goal && e.state !== 'pre') {
      const dh = (e.h.sc || 0) - (p.h || 0), da = (e.a.sc || 0) - (p.a || 0);
      if (dh > 0 || da > 0) {
        const side = dh > 0 ? 'h' : 'a';
        const g = e.goals.filter((x) => x.s === side).pop();
        const teamId = side === 'h' ? e.h.id : e.a.id;
        const ours = s.cfg.fb.tm.some((t) => t.id === teamId);
        pushEvent(s, { k: 'goal', ti: `GOAL! ${score}`, tx: g ? `${g.who} ${g.min}` : (side === 'h' ? e.h.n : e.a.n), m: ours ? 1 : (e.mine ? -1 : 0) });
      } else if (dh < 0 || da < 0) {
        pushEvent(s, { k: 'var', ti: 'Goal ruled out', tx: score, m: 0 });
      }
    }
    if (al.res && p.state !== 'post' && e.state === 'post') pushEvent(s, { k: 'ft', ti: `FT ${score}`, tx: e.detail, m: e.mine ? 1 : 0 });
  }
}

function inningsOf(sc) { return String(sc || '').split('&').length; }

function diffCricket(s, prev, cur) {
  const al = s.cfg.al;
  for (const m of cur) {
    const p = prev.get(m.id);
    if (!p) continue;
    if (!m.mine && !al.any) continue;
    if (al.start && p.state === 'pre' && m.state === 'in') pushEvent(s, { k: 'start', ti: 'Match started', tx: m.title, m: m.mine ? 1 : 0 });
    if (al.wkt && m.state === 'in') {
      m.t.forEach((side, i) => {
        const ps = p.t[i];
        if (!ps || side.w === null || ps.w === null) return;
        if (inningsOf(side.sc) === inningsOf(ps.sc) && side.w > ps.w && side.w <= 10) {
          const ours = s.cfg.cr.tm.some((t) => (t.id && t.id === side.id) || (!t.id && t.abbr === side.a));
          pushEvent(s, { k: 'wkt', ti: 'WICKET!', tx: clip(`${side.a} ${side.sc}${side.ov ? ` (${side.ov})` : ''}`, 30), m: ours ? -1 : (m.mine ? 1 : 0) });
        }
      });
    }
    if (al.res && p.state !== 'post' && m.state === 'post') {
      const won = m.t.find((x) => x.win);
      const ours = won && s.cfg.cr.tm.some((t) => (t.id && t.id === won.id) || (!t.id && t.abbr === won.a));
      pushEvent(s, { k: 'res', ti: 'Result', tx: clip(m.status || m.title, 60), m: ours ? 1 : (won && m.mine ? -1 : 0) });
    }
  }
}

// ---------------------------------------------------------------- build
function bump(s) {
  s.v++;
  s.body = JSON.stringify({ ...s.feed, v: s.v, b: BOOT, t: Math.floor(Date.now() / 1000), ev: s.events });
  for (const w of s.waiters) w();
  s.waiters.clear();
}

async function build(s) {
  if (s.building) return s.building;
  s.building = (async () => {
    const c = s.cfg;
    const imperial = c.units === 'imperial';
    const tasks = {
      wx: c.wx && c.loc ? weather.weather(c.loc.lat, c.loc.lon, imperial) : null,
      aq: c.aq && c.loc ? weather.air(c.loc.lat, c.loc.lon) : null,
      fb: footballFor(c),
      tb: Promise.all(c.fb.tb.map((lg) => football.standings(lg).catch(() => null))),
      cr: c.cr.tm.length || c.cr.all ? cricket.forSub(c.cr) : null,
      mk: c.mk.cr.length || c.mk.st.length || c.mk.fx.length ? markets.forSub(c.mk) : null,
      nw: c.nw ? news.headlines(c.nw) : null,
      qt: c.qt ? quote() : null,
      ic: c.ic ? ical.upcoming(c.ic, c.tz) : null,
    };
    const keys = Object.keys(tasks);
    const res = await Promise.allSettled(keys.map((k) => tasks[k]));
    const old = s.feed || {};
    const f = {};
    const err = [];
    keys.forEach((k, i) => {
      const r = res[i];
      if (tasks[k] === null) return;
      if (r.status === 'fulfilled' && r.value !== null && r.value !== undefined) f[k] = r.value;
      else { if (old[k] !== undefined) f[k] = old[k]; err.push(k); }
    });
    // Alerts from raw snapshots, then shrink to the device format.
    const fbRaw = Array.isArray(f.fb) && f.fb.length && f.fb[0].goals ? f.fb : null;
    const crRaw = Array.isArray(f.cr) && f.cr.length && Array.isArray(f.cr[0].t) ? f.cr : null;
    if (s.prev) {
      if (fbRaw) diffFootball(s, s.prev.fb, fbRaw);
      if (crRaw) diffCricket(s, s.prev.cr, crRaw);
    }
    const prevFb = fbRaw ? new Map(fbRaw.map((e) => [e.id, { state: e.state, h: e.h.sc || 0, a: e.a.sc || 0 }])) : (s.prev ? s.prev.fb : new Map());
    const prevCr = crRaw ? new Map(crRaw.map((m) => [m.id, { state: m.state, t: m.t.map((x) => ({ w: x.w, sc: x.sc })) }])) : (s.prev ? s.prev.cr : new Map());
    s.prev = { fb: prevFb, cr: prevCr };
    if (fbRaw) f.fb = fbRaw.map(fbOut);
    if (crRaw) f.cr = crRaw.map(crOut);
    if (Array.isArray(f.tb)) f.tb = f.tb.filter(Boolean).map((t) => ({
      lg: t.name, lid: t.lg,
      r: t.rows.map((r) => [r.rank, r.a, r.pts, r.gp, r.gd, c.fb.tm.some((x) => x.id === r.id) ? 1 : 0]),
    }));
    s.hot = (f.fb || []).some((e) => e.s === 'in') || (f.cr || []).some((m) => m.s === 'in');
    f.err = err;
    s.feed = f;
    const content = JSON.stringify(f);
    const nEv = s.events.length ? s.events[s.events.length - 1].q : 0;
    if (content !== s.content || nEv !== s.lastEvQ) {
      s.content = content;
      s.lastEvQ = nEv;
      bump(s);
    }
  })().finally(() => { s.building = null; });
  return s.building;
}

// Rebuild active feeds. Sources are cached with their own TTLs, so this is
// cheap; live matches refresh every 15-20 s, everything else much slower.
async function tick() {
  const now = Date.now();
  for (const [id, s] of subs) {
    if (now - s.lastSeen > 30 * 60000 && !s.waiters.size) { subs.delete(id); continue; }
    if (now - s.lastSeen > 10 * 60000) continue;
    const every = s.hot ? 5000 : 30000;
    if (s.builtAt && now - s.builtAt < every) continue;
    s.builtAt = now;
    build(s).catch((e) => console.error(`[feed ${id}]`, e.message));
  }
}

function waitFor(s, ms) {
  return new Promise((resolve) => {
    let done = false;
    const fin = (changed) => { if (done) return; done = true; clearTimeout(t); s.waiters.delete(w); resolve(changed); };
    const w = () => fin(true);
    const t = setTimeout(() => fin(false), ms);
    s.waiters.add(w);
  });
}

module.exports = { BOOT, subs, normalizeSub, subId, register, build, tick, waitFor, broadcast, pushEvent, bump, footballFor, diffFootball, diffCricket, fbOut, crOut, num };
