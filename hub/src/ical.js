'use strict';
// iCal (.ics) upcoming events with repeating-event (RRULE) expansion.
// Handles DTSTART/DTEND with TZID, UTC and floating times, all-day events,
// FREQ=DAILY|WEEKLY|MONTHLY|YEARLY, INTERVAL, COUNT, UNTIL, BYDAY (weekly and
// monthly "2TU"), BYMONTHDAY and EXDATE. Good enough for Google/Outlook/Apple feeds.
const { getText, cached } = require('./http');
const { clip } = require('./util');

function unfold(text) {
  return text.replace(/\r\n/g, '\n').replace(/\n[ \t]/g, '');
}

function parseProps(block) {
  const props = [];
  for (const line of block.split('\n')) {
    const i = line.indexOf(':');
    if (i < 0) continue;
    const head = line.slice(0, i);
    const value = line.slice(i + 1);
    const [name, ...params] = head.split(';');
    const p = {};
    for (const x of params) { const [k, v] = x.split('='); p[(k || '').toUpperCase()] = (v || '').replace(/"/g, ''); }
    props.push({ name: name.toUpperCase(), params: p, value });
  }
  return props;
}

// Offset (ms) of a time zone at a given UTC instant, via Intl.
function tzOffset(tz, utcMs) {
  try {
    const f = new Intl.DateTimeFormat('en-US', { timeZone: tz, hourCycle: 'h23', year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', second: '2-digit' });
    const parts = Object.fromEntries(f.formatToParts(new Date(utcMs)).map((x) => [x.type, x.value]));
    const asUtc = Date.UTC(+parts.year, +parts.month - 1, +parts.day, +parts.hour % 24, +parts.minute, +parts.second);
    return asUtc - utcMs;
  } catch (_) {
    return 0;
  }
}

// Wall-clock time in tz -> UTC ms.
function zoned(y, mo, d, h, mi, s, tz) {
  const guess = Date.UTC(y, mo, d, h, mi, s);
  if (!tz) return guess;
  let t = guess - tzOffset(tz, guess);
  t = guess - tzOffset(tz, t); // second pass settles DST edges
  return t;
}

// Returns { ms, allDay, wall:{y,mo,d,h,mi,s}, tz }
function parseDate(prop, defTz) {
  if (!prop) return null;
  const v = prop.value.trim();
  const m = v.match(/^(\d{4})(\d{2})(\d{2})(?:T(\d{2})(\d{2})(\d{2})?(Z)?)?$/);
  if (!m) return null;
  const [, y, mo, d, h, mi, s, z] = m;
  const allDay = prop.params.VALUE === 'DATE' || h === undefined;
  const wall = { y: +y, mo: +mo - 1, d: +d, h: +(h || 0), mi: +(mi || 0), s: +(s || 0) };
  const tz = z ? 'UTC' : prop.params.TZID || defTz || '';
  const ms = z ? Date.UTC(wall.y, wall.mo, wall.d, wall.h, wall.mi, wall.s) : zoned(wall.y, wall.mo, wall.d, wall.h, wall.mi, wall.s, allDay ? defTz : tz);
  return { ms, allDay, wall, tz: allDay ? defTz : tz };
}

const DAYS = { SU: 0, MO: 1, TU: 2, WE: 3, TH: 4, FR: 5, SA: 6 };

function parseRule(s) {
  const r = {};
  for (const part of s.split(';')) { const [k, v] = part.split('='); r[k.toUpperCase()] = v; }
  return r;
}

// Generate occurrence start times (UTC ms) of an event within [from, to].
function occurrences(start, rule, exdates, from, to, limit = 400) {
  const out = [];
  if (!rule) {
    if (start.ms >= from - 86400000 && start.ms <= to) out.push(start.ms);
    return out;
  }
  const r = parseRule(rule);
  const freq = r.FREQ;
  const interval = Math.max(1, parseInt(r.INTERVAL || '1', 10));
  const count = r.COUNT ? parseInt(r.COUNT, 10) : Infinity;
  let until = Infinity;
  if (r.UNTIL) {
    const u = parseDate({ value: r.UNTIL, params: {} }, start.tz);
    if (u) until = u.ms + (u.allDay ? 86400000 - 1 : 0);
  }
  const byday = (r.BYDAY || '').split(',').filter(Boolean).map((x) => {
    const mm = x.match(/^([+-]?\d+)?([A-Z]{2})$/);
    return mm ? { n: mm[1] ? parseInt(mm[1], 10) : 0, wd: DAYS[mm[2]] } : null;
  }).filter(Boolean);
  const bymd = (r.BYMONTHDAY || '').split(',').filter(Boolean).map(Number);
  const w = start.wall;
  const tz = start.tz;
  const mk = (y, mo, d) => zoned(y, mo, d, w.h, w.mi, w.s, tz);
  let n = 0;
  const push = (ms) => {
    if (ms < start.ms) return true;
    if (ms > until || n >= count) return false;
    n++;
    if (ms >= from - 86400000 && ms <= to && !exdates.has(ms)) out.push(ms);
    return ms <= to;
  };
  // Walk period by period from DTSTART. Bounded so a bad feed cannot spin.
  for (let i = 0, guard = 0; guard < 5000 && out.length < limit; i += interval, guard++) {
    let cont = true;
    if (freq === 'DAILY') {
      const d = new Date(Date.UTC(w.y, w.mo, w.d + i));
      cont = push(mk(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate()));
    } else if (freq === 'WEEKLY') {
      const base = new Date(Date.UTC(w.y, w.mo, w.d + i * 7));
      const weekStart = new Date(base.getTime() - ((base.getUTCDay() + 6) % 7) * 86400000); // Monday
      const days = byday.length ? byday.map((b) => b.wd) : [new Date(Date.UTC(w.y, w.mo, w.d)).getUTCDay()];
      const sorted = [...days].sort((a, b) => ((a + 6) % 7) - ((b + 6) % 7));
      for (const wd of sorted) {
        const d = new Date(weekStart.getTime() + ((wd + 6) % 7) * 86400000);
        if (!(cont = push(mk(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate())))) break;
      }
    } else if (freq === 'MONTHLY') {
      const first = new Date(Date.UTC(w.y, w.mo + i, 1));
      const y = first.getUTCFullYear(), mo = first.getUTCMonth();
      const dim = new Date(Date.UTC(y, mo + 1, 0)).getUTCDate();
      const days = [];
      if (byday.length) {
        for (const b of byday) {
          const all = [];
          for (let d = 1; d <= dim; d++) if (new Date(Date.UTC(y, mo, d)).getUTCDay() === b.wd) all.push(d);
          if (b.n > 0 && all[b.n - 1]) days.push(all[b.n - 1]);
          else if (b.n < 0 && all[all.length + b.n]) days.push(all[all.length + b.n]);
          else if (b.n === 0) days.push(...all);
        }
      } else if (bymd.length) {
        for (const md of bymd) days.push(md > 0 ? md : dim + md + 1);
      } else days.push(w.d);
      for (const d of days.filter((x) => x >= 1 && x <= dim).sort((a, b) => a - b)) {
        if (!(cont = push(mk(y, mo, d)))) break;
      }
    } else if (freq === 'YEARLY') {
      const y = w.y + i;
      const dim = new Date(Date.UTC(y, w.mo + 1, 0)).getUTCDate();
      if (w.d <= dim) cont = push(mk(y, w.mo, w.d));
    } else {
      push(start.ms);
      break;
    }
    if (!cont) break;
  }
  return out;
}

function parseIcs(text, { from = Date.now(), days = 7, tz = 'UTC' } = {}) {
  const src = unfold(String(text || ''));
  const calTz = (src.match(/^X-WR-TIMEZONE:(.+)$/m) || [])[1];
  const defTz = (calTz || tz || 'UTC').trim();
  const to = from + days * 86400000;
  const events = [];
  const overrides = new Map(); // UID -> Set(recurrence ms) replaced by own VEVENT
  const blocks = src.split('BEGIN:VEVENT').slice(1).map((b) => b.split('END:VEVENT')[0]);
  const parsed = blocks.map(parseProps);
  for (const props of parsed) {
    const rid = props.find((p) => p.name === 'RECURRENCE-ID');
    const uid = (props.find((p) => p.name === 'UID') || {}).value;
    if (rid && uid) {
      const d = parseDate(rid, defTz);
      if (d) { if (!overrides.has(uid)) overrides.set(uid, new Set()); overrides.get(uid).add(d.ms); }
    }
  }
  for (const props of parsed) {
    const get = (n) => props.find((p) => p.name === n);
    if ((get('STATUS') || {}).value === 'CANCELLED') continue;
    const start = parseDate(get('DTSTART'), defTz);
    if (!start) continue;
    const end = parseDate(get('DTEND'), defTz);
    const dur = end ? end.ms - start.ms : start.allDay ? 86400000 : 0;
    const summary = clip((get('SUMMARY') || {}).value?.replace(/\\,/g, ',').replace(/\\;/g, ';').replace(/\\n/gi, ' ') || 'Event', 40);
    const uid = (get('UID') || {}).value;
    const ex = new Set();
    for (const p of props.filter((x) => x.name === 'EXDATE')) {
      for (const v of p.value.split(',')) { const d = parseDate({ value: v, params: p.params }, defTz); if (d) ex.add(d.ms); }
    }
    const isOverride = !!get('RECURRENCE-ID');
    const rrule = isOverride ? null : (get('RRULE') || {}).value;
    if (rrule && uid && overrides.has(uid)) for (const ms of overrides.get(uid)) ex.add(ms);
    for (const ms of occurrences(start, rrule, ex, from, to)) {
      if (ms + Math.max(dur, 1) < from) continue; // already over
      events.push({ t: Math.floor(ms / 1000), e: Math.floor((ms + dur) / 1000), d: summary, ad: start.allDay ? 1 : 0 });
    }
  }
  events.sort((a, b) => a.t - b.t);
  return events;
}

async function upcoming(url, tz) {
  if (!/^(https?|webcal):\/\//i.test(url || '')) return [];
  const u = url.replace(/^webcal:/i, 'https:');
  const text = await cached(`ic:${u}`, 15 * 60000, () => getText(u, { timeout: 12000 }));
  return parseIcs(text, { tz }).slice(0, 8);
}

module.exports = { parseIcs, occurrences, parseDate, zoned, upcoming };
