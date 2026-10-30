'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fx = require('./fixtures');
const util = require('../src/util');
const football = require('../src/football');
const cricket = require('../src/cricket');
const news = require('../src/news');
const ical = require('../src/ical');
const feed = require('../src/feed');

test('ascii folds accents and smart punctuation for the OLED font', () => {
  assert.equal(util.ascii('Mbappé – “Kylían”'), 'Mbappe - "Kylian"');
  assert.equal(util.ascii('Müller ß'), 'Muller ss');
  assert.equal(util.clip('The quick brown fox jumps over', 15), 'The quick~');
});

test('spark scales any series to 0..15', () => {
  const s = util.spark([1, 2, 3, 4, 5], 5);
  assert.deepEqual(s, [0, 4, 8, 11, 15]);
  assert.deepEqual(util.spark([5]), []);
});

test('football event: scores, goals, own goal credited to the other side', () => {
  const ev = fx.soccerEvent({
    hs: 2, as: 1,
    goals: [
      { team: '359', name: 'Bukayo Saka', short: 'B. Saka', min: "12'" },
      { team: '359', name: 'Ben White', short: 'B. White', min: "40'", own: true },
      { team: '363', name: 'Cole Palmer', short: 'C. Palmer', min: "55'", pen: true },
    ],
  });
  const e = football.parseEvent(ev, 'eng.1');
  assert.equal(e.state, 'in');
  assert.equal(e.h.a, 'ARS');
  assert.equal(e.h.sc, 2);
  assert.equal(e.a.sc, 1);
  assert.deepEqual(e.goals.map((g) => g.s), ['h', 'a', 'a']);
  assert.equal(e.goals[1].who, 'B. White (og)');
  assert.equal(e.goals[2].who, 'C. Palmer (p)');
});

test('football scheduled match has no score', () => {
  const ev = fx.soccerEvent({ state: 'pre' });
  ev.competitions[0].competitors.forEach((c) => { delete c.score; });
  const e = football.parseEvent(ev, 'eng.1');
  assert.equal(e.h.sc, null);
});

test('cricket score strings', () => {
  assert.deepEqual(cricket.parseScore('176/4 (18.3/20 ov, target 199)'), { main: '176/4', runs: 176, wkts: 4, overs: '18.3', target: 199 });
  const t = cricket.parseScore('320 & 145/3 (32.4 ov)');
  assert.equal(t.main, '320 & 145/3');
  assert.equal(t.runs, 145);
  assert.equal(t.wkts, 3);
  assert.equal(cricket.parseScore('287').wkts, 10);
  assert.equal(cricket.parseScore(''), null);
  assert.equal(cricket.balls('18.3'), 111);
  assert.equal(cricket.fmtOvers(16.2), '16.2');
  assert.equal(cricket.fmtOvers(20), '20');
});

test('cricket header event + series enrichment + chase maths', () => {
  const h = fx.cricketHeader();
  const lg = h.sports[0].leagues[0];
  const m = cricket.headerEvent(lg.events[0], lg);
  assert.equal(m.state, 'in');
  assert.equal(m.fmt, 'T20');
  assert.equal(m.t[0].a, 'IND');
  assert.equal(m.t[0].sc, '145/4');
  cricket.applySeries(m, fx.cricketSeries().events[0]);
  assert.equal(m.t[0].bat, true);
  assert.equal(m.t[0].ov, '16.2');
  cricket.derive(m);
  assert.equal(m.crr, 8.9);
  assert.equal(m.rrr, 12);
  assert.equal(m.need, '44 off 22b');
});

test('cricket team matching is by id, never by substring', () => {
  const lg = fx.cricketHeader().sports[0].leagues[0];
  const m = cricket.headerEvent(lg.events[0], lg);
  assert.equal(cricket.isMine(m, [{ id: '6' }]), true);
  assert.equal(cricket.isMine(m, [{ id: '60' }]), false);
  assert.equal(cricket.isMine(m, [{ id: '', abbr: 'AUS' }]), true);
  assert.equal(cricket.isMine(m, [{ id: '', abbr: 'AU' }]), false);
});

test('rss parsing decodes entities and strips publisher suffix', () => {
  const items = news.parseFeed(fx.rss);
  assert.equal(items.length, 3);
  assert.equal(items[0].h, 'Leaders meet for climate talks');
  assert.equal(items[1].h, 'Scientists find "new" species & more');
  assert.equal(items[2].h, "Cafe owner's big win");
  assert.equal(items[2].s, 'Local Paper');
});

test('ical: weekly BYDAY with TZID, EXDATE and all-day events', () => {
  const ics = [
    'BEGIN:VCALENDAR',
    'BEGIN:VEVENT', 'UID:a', 'SUMMARY:Standup', 'DTSTART;TZID=Asia/Kolkata:20261026T093000', 'DTEND;TZID=Asia/Kolkata:20261026T094500',
    'RRULE:FREQ=WEEKLY;BYDAY=MO,WE,FR', 'EXDATE;TZID=Asia/Kolkata:20261030T093000', 'END:VEVENT',
    'BEGIN:VEVENT', 'UID:b', 'SUMMARY:Diwali', 'DTSTART;VALUE=DATE:20261108', 'DTEND;VALUE=DATE:20261109', 'END:VEVENT',
    'BEGIN:VEVENT', 'UID:c', 'SUMMARY:Rent', 'DTSTART:20260101T043000Z', 'RRULE:FREQ=MONTHLY;BYMONTHDAY=1', 'END:VEVENT',
    'END:VCALENDAR',
  ].join('\r\n');
  const from = Date.UTC(2026, 9, 29, 0, 0); // Thu 29 Oct 2026
  const ev = ical.parseIcs(ics, { from, days: 12, tz: 'Asia/Kolkata' });
  const standups = ev.filter((e) => e.d === 'Standup').map((e) => new Date(e.t * 1000).toISOString());
  // Fri 30 is excluded; Mon 2, Wed 4, Fri 6, Mon 9 Nov at 09:30 IST = 04:00Z
  assert.deepEqual(standups, ['2026-11-02T04:00:00.000Z', '2026-11-04T04:00:00.000Z', '2026-11-06T04:00:00.000Z', '2026-11-09T04:00:00.000Z']);
  const diwali = ev.find((e) => e.d === 'Diwali');
  assert.equal(diwali.ad, 1);
  const rent = ev.filter((e) => e.d === 'Rent');
  assert.equal(rent.length, 1);
  assert.equal(new Date(rent[0].t * 1000).toISOString(), '2026-11-01T04:30:00.000Z');
});

test('ical: monthly nth weekday and COUNT', () => {
  const ics = 'BEGIN:VEVENT\nUID:x\nSUMMARY:Club\nDTSTART:20260113T180000Z\nRRULE:FREQ=MONTHLY;BYDAY=2TU;COUNT=12\nEND:VEVENT';
  const ev = ical.parseIcs(ics, { from: Date.UTC(2026, 10, 1), days: 40 });
  assert.deepEqual(ev.map((e) => new Date(e.t * 1000).toISOString().slice(0, 10)), ['2026-11-10', '2026-12-08']);
});

test('subscription is sanitised and stable', () => {
  const a = feed.normalizeSub({ fb: { lg: ['eng.1', 'bad league!'], tm: [{ id: '359', lg: 'eng.1' }] }, loc: { lat: '12.97', lon: '77.59' }, junk: 1 });
  assert.deepEqual(a.fb.lg, ['eng.1']);
  assert.equal(a.loc.lat, 12.97);
  assert.equal(a.junk, undefined);
  assert.equal(feed.subId(a), feed.subId(feed.normalizeSub({ loc: { lat: 12.97, lon: 77.59 }, fb: { lg: 'eng.1', tm: [{ id: '359', lg: 'eng.1' }] } })));
});

test('alerts: goal for my team, VAR, full time, only once each', () => {
  const s = feed.register({ name: 'alerts-test', fb: { lg: ['eng.1'], tm: [{ id: '359', lg: 'eng.1' }] } });
  const mk = (o) => ({ ...football.parseEvent(fx.soccerEvent(o), 'eng.1'), mine: true });
  const snap = (e) => new Map([[e.id, { state: e.state, h: e.h.sc, a: e.a.sc }]]);
  const e0 = mk({ hs: 0, as: 0 });
  const e1 = mk({ hs: 1, as: 0, goals: [{ team: '359', name: 'Bukayo Saka', short: 'B. Saka', min: "34'" }] });
  feed.diffFootball(s, snap(e0), [e1]);
  assert.equal(s.events.length, 1);
  assert.equal(s.events[0].k, 'goal');
  assert.equal(s.events[0].ti, 'GOAL! ARS 1-0 CHE');
  assert.equal(s.events[0].tx, "B. Saka 34'");
  assert.equal(s.events[0].m, 1);
  feed.diffFootball(s, snap(e1), [e1]);
  assert.equal(s.events.length, 1, 'no repeat when nothing changed');
  feed.diffFootball(s, snap(e1), [mk({ hs: 0, as: 0 })]);
  assert.equal(s.events[1].k, 'var');
  feed.diffFootball(s, snap(e1), [mk({ state: 'post', hs: 1, as: 0, detail: 'FT' })]);
  assert.equal(s.events[2].k, 'ft');
});

test('alerts: wicket only within the same innings', () => {
  const s = feed.register({ name: 'wkt-test', cr: { tm: [{ id: '6' }] } });
  const lg = fx.cricketHeader().sports[0].leagues[0];
  const at = (ind) => ({ ...cricket.headerEvent(fx.cricketHeader({ ind }).sports[0].leagues[0].events[0], lg), mine: true });
  const snap = (m) => new Map([[m.id, { state: m.state, t: m.t.map((x) => ({ w: x.w, sc: x.sc })) }]]);
  const a = at('145/4 (16.2 ov)');
  const b = at('146/5 (16.4 ov)');
  feed.diffCricket(s, snap(a), [b]);
  assert.equal(s.events.length, 1);
  assert.equal(s.events[0].k, 'wkt');
  assert.match(s.events[0].tx, /IND 146\/5 \(16\.4\)/);
  assert.equal(s.events[0].m, -1, 'our wicket fell = sad');
  // New innings: 320 all out then 12/1 is not a wicket "increase" from 10
  const c = at('320 (80 ov)');
  const d = at('320 & 12/1 (4 ov)');
  feed.diffCricket(s, snap(c), [d]);
  assert.equal(s.events.length, 1);
});
