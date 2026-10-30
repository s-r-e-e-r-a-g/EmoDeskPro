'use strict';
// Cricket from ESPN's public APIs (no key).
//
//   header:     site.web.api.espn.com/apis/personalized/v2/scoreboard/header?sport=cricket
//               every active series with its matches, status and summary text
//   scoreboard: site.api.espn.com/apis/site/v2/sports/cricket/{seriesId}/scoreboard
//               per series, with structured innings (runs, wickets, overs, isBatting)
//
// The header is polled every 20 s while a followed match is live (3 min
// otherwise). Live matches are then enriched from their series scoreboard.
const { getJson, cached } = require('./http');
const { clip, int, num, unix } = require('./util');

const HEADER = 'https://site.web.api.espn.com/apis/personalized/v2/scoreboard/header?sport=cricket&lang=en&region=in';
const SERIES = 'https://site.api.espn.com/apis/site/v2/sports/cricket';

let hot = false; // anything followed is live -> poll fast

// "320 & 145/3 (32.4 ov, target 280)" -> { main, runs, wkts, overs, target }
function parseScore(str) {
  const s = String(str || '').trim();
  if (!s) return null;
  const paren = (s.match(/\(([^)]*)\)/) || [])[1] || '';
  const main = s.replace(/\([^)]*\)/g, '').replace(/\s+/g, ' ').trim();
  const last = main.split('&').pop().trim();
  const m = last.match(/(\d+)(?:\s*\/\s*(\d+))?\s*(d|dec)?/i);
  if (!m) return { main, runs: null, wkts: null, overs: '', target: 0 };
  const ov = paren.match(/(\d+(?:\.\d)?)\s*(?:\/\s*\d+\s*)?ov/i);
  const tg = paren.match(/target\s*(\d+)/i) || s.match(/target\s*(\d+)/i);
  return {
    main,
    runs: int(m[1]),
    wkts: m[2] !== undefined ? int(m[2]) : 10,
    overs: ov ? ov[1] : '',
    target: tg ? int(tg[1]) : 0,
  };
}

// Overs as balls ("18.3" -> 111) for run-rate maths.
function balls(overs) {
  const o = String(overs || '');
  if (!o) return 0;
  const [w, b = '0'] = o.split('.');
  return int(w) * 6 + Math.min(5, int(b));
}

function fmtOvers(o) {
  if (o === undefined || o === null || o === '') return '';
  const n = num(o);
  const w = Math.floor(n);
  let b = Math.round((n - w) * 10);
  if (b >= 6) return `${w + 1}`;
  return b ? `${w}.${b}` : `${w}`;
}

function format(name) {
  const n = (name || '').toLowerCase();
  if (/t20|twenty20/.test(n)) return 'T20';
  if (/odi|one-day|one day|50-over/.test(n)) return 'ODI';
  if (/test|first-class|4-day|5-day|ranji|shield|county/.test(n)) return 'TEST';
  if (/t10|hundred/.test(n)) return 'T10';
  return '';
}

function headerEvent(e, lg) {
  const comps = e.competitors || [];
  const st = (e.fullStatus || {}).type || {};
  const state = st.state || (['pre', 'in', 'post'].includes(e.status) ? e.status : 'pre');
  const teams = comps.slice(0, 2).map((c) => {
    const p = parseScore(c.score);
    return {
      id: String(c.id || ''),
      a: clip(c.abbreviation || c.shortDisplayName || c.displayName || '?', 5).toUpperCase(),
      n: clip(c.displayName || c.name || '', 20),
      sc: p ? clip(p.main, 18) : '',
      r: p ? p.runs : null,
      w: p ? p.wkts : null,
      ov: p ? p.overs : '',
      tg: p ? p.target : 0,
      bat: !!(c.isBatting || c.possession),
      win: !!c.winner,
    };
  });
  while (teams.length < 2) teams.push({ id: '', a: '?', n: '', sc: '', r: null, w: null, ov: '', tg: 0, bat: false, win: false });
  const desc = e.description || e.note || e.shortName || '';
  return {
    id: String(e.id),
    ser: String(lg.id || ''),
    serName: clip(lg.name || lg.abbreviation || '', 26),
    title: clip(e.shortName || e.name || `${teams[0].a} v ${teams[1].a}`, 26),
    fmt: format(`${e.name} ${desc} ${lg.name}`),
    state,
    status: clip(e.summary || st.detail || st.shortDetail || st.description || '', 60),
    start: unix(e.date),
    t: teams,
  };
}

async function header() {
  return cached('cr:header', hot ? 20000 : 180000, async () => {
    const j = await getJson(HEADER);
    const out = [];
    for (const sport of j.sports || []) {
      for (const lg of sport.leagues || []) {
        for (const e of lg.events || []) out.push(headerEvent(e, lg));
      }
    }
    return out;
  });
}

// Structured innings for a live match from its series scoreboard.
function applySeries(match, ev) {
  const comp = (ev.competitions || [])[0] || {};
  const cs = comp.competitors || [];
  for (const side of match.t) {
    const c = cs.find((x) => String((x.team || {}).id || x.id) === side.id);
    if (!c) continue;
    const ls = (c.linescores || []).filter((l) => l && (l.runs !== undefined || l.wickets !== undefined));
    if (ls.length) {
      const cur = ls[ls.length - 1];
      side.r = int(cur.runs);
      side.w = int(cur.wickets);
      side.ov = fmtOvers(cur.overs);
      side.bat = !!cur.isBatting;
      const parts = ls.map((l) => (int(l.wickets) >= 10 ? `${int(l.runs)}` : `${int(l.runs)}/${int(l.wickets)}`));
      side.sc = clip(parts.join(' & '), 18);
    }
  }
  const st = (comp.status || {}).type || {};
  const note = ((comp.notes || [])[0] || {}).text;
  if (!match.status && (note || st.detail)) match.status = clip(note || st.detail, 60);
}

async function enrich(match) {
  if (!match.ser) return match;
  try {
    const events = await cached(`cr:series:${match.ser}`, 20000, async () => {
      const j = await getJson(`${SERIES}/${match.ser}/scoreboard`);
      return j.events || [];
    });
    const ev = events.find((x) => String(x.id) === match.id);
    if (ev) applySeries(match, ev);
  } catch (_) { /* header data is still good enough */ }
  return match;
}

// Run rates and the chase equation, derived from scores + summary text.
function derive(m) {
  const bat = m.t.find((x) => x.bat) || null;
  m.crr = 0; m.rrr = 0; m.need = '';
  if (!bat || bat.r === null) return m;
  const b = balls(bat.ov);
  if (b > 0) m.crr = Math.round((bat.r * 6 * 10) / b) / 10;
  const need = (m.status.match(/need\s+(\d+)\s+runs?\s+(?:in|from|off)\s+(\d+)\s+(balls?|overs?)/i) || []);
  if (need.length) {
    const runs = int(need[1]);
    const left = /over/i.test(need[3]) ? int(need[2]) * 6 : int(need[2]);
    if (left > 0) m.rrr = Math.round((runs * 6 * 10) / left) / 10;
    m.need = `${runs} off ${need[2]}${/over/i.test(need[3]) ? 'ov' : 'b'}`;
  }
  return m;
}

function isMine(m, teams) {
  if (!teams || !teams.length) return false;
  return m.t.some((side) => teams.some((t) => {
    const id = typeof t === 'object' ? String(t.id || '') : String(t || '');
    const abbr = String(t.abbr || '').toUpperCase();
    return (id && id === side.id) || (!id && abbr && abbr === side.a);
  }));
}

// Matches for one subscription: followed teams first, then (optionally) every
// other live match. Upcoming/recent matches only for followed teams.
async function forSub(sub) {
  const teams = (sub.tm || []).map((t) => (typeof t === 'object' ? t : { id: String(t) }));
  const all = await header();
  const now = Date.now() / 1000;
  const picked = [];
  for (const m of all) {
    const mine = isMine(m, teams);
    const recent = m.state === 'post' && now - m.start < 4 * 86400;
    const soon = m.state === 'pre' && m.start - now < 7 * 86400;
    if (mine && (m.state === 'in' || recent || soon)) picked.push({ ...m, mine: true });
    else if (!mine && sub.all && m.state === 'in') picked.push({ ...m, mine: false });
  }
  const rank = (m) => (m.state === 'in' ? 0 : m.state === 'pre' ? 1 : 2) + (m.mine ? 0 : 3);
  picked.sort((x, y) => rank(x) - rank(y) || (x.state === 'post' ? y.start - x.start : x.start - y.start));
  const out = picked.slice(0, 6);
  // Keep at most one upcoming and one finished match per followed team so the
  // card does not fill with next week's fixtures.
  const seen = new Set();
  const trimmed = out.filter((m) => {
    if (m.state === 'in') return true;
    const k = `${m.state}:${m.t.map((s) => s.id).sort().join('-')}`;
    if (seen.has(k)) return false;
    seen.add(k);
    return true;
  });
  hot = trimmed.some((m) => m.state === 'in' || (m.state === 'pre' && m.start - now < 900));
  await Promise.all(trimmed.filter((m) => m.state === 'in').map(enrich));
  return trimmed.map(derive);
}

async function teamList() {
  const all = await header().catch(() => []);
  const map = new Map();
  for (const m of all) for (const t of m.t) if (t.id && !map.has(t.id)) map.set(t.id, { id: t.id, abbr: t.a, name: t.n });
  return [...map.values()].sort((x, y) => x.name.localeCompare(y.name));
}

module.exports = { parseScore, balls, fmtOvers, headerEvent, applySeries, derive, isMine, forSub, teamList, header };
