'use strict';
// End to end: real HTTP server, upstream APIs replaced by fixtures. Checks the
// device flow (subscribe -> long-poll -> instant wake on a goal -> 304 when idle).
const test = require('node:test');
const assert = require('node:assert/strict');
const fx = require('./fixtures');

let goals = 0;
const calls = [];
global.fetch = async (url) => {
  const u = String(url);
  calls.push(u);
  const json = (o) => new Response(JSON.stringify(o), { status: 200, headers: { 'content-type': 'application/json' } });
  if (u.includes('/soccer/eng.1/scoreboard')) {
    const g = Array.from({ length: goals }, (_, i) => ({ team: '359', name: 'Bukayo Saka', short: 'B. Saka', min: `${20 + i}'` }));
    return json({ events: [fx.soccerEvent({ hs: goals, as: 0, goals: g })] });
  }
  if (u.includes('/soccer/eng.1/standings')) {
    return json({ children: [{ name: 'Premier League', standings: { entries: [
      { team: { id: '359', abbreviation: 'ARS' }, stats: [{ name: 'rank', value: 1 }, { name: 'points', value: 25 }, { name: 'gamesPlayed', value: 10 }, { name: 'pointDifferential', value: 14 }] },
      { team: { id: '363', abbreviation: 'CHE' }, stats: [{ name: 'rank', value: 2 }, { name: 'points', value: 22 }, { name: 'gamesPlayed', value: 10 }, { name: 'pointDifferential', value: 9 }] },
    ] } }] });
  }
  if (u.includes('scoreboard/header')) return json(fx.cricketHeader());
  if (u.includes('/sports/cricket/')) return json(fx.cricketSeries());
  if (u.includes('air-quality')) return json({ current: { us_aqi: 88, pm2_5: 30.1, pm10: 50, uv_index: 4 } });
  if (u.includes('api.open-meteo.com')) return json(fx.openMeteo);
  if (u.includes('zenquotes')) return json([{ q: 'Stay hungry.', a: 'Someone' }]);
  if (u.includes('bbci')) return new Response(fx.rss, { status: 200 });
  return new Response('nope', { status: 404 });
};

const { server } = require('../server');
const feed = require('../src/feed');

function req(port, path, { method = 'GET', body } = {}) {
  return fetchReal(`http://127.0.0.1:${port}${path}`, { method, body: body ? JSON.stringify(body) : undefined, headers: { 'Content-Type': 'application/json' } });
}
// Plain http client, since global.fetch is mocked.
const http = require('http');
function fetchReal(url, { method, body, headers }) {
  return new Promise((resolve, reject) => {
    const r = http.request(url, { method, headers }, (res) => {
      let d = '';
      res.on('data', (c) => { d += c; });
      res.on('end', () => resolve({ status: res.statusCode, body: d ? JSON.parse(d) : null }));
    });
    r.on('error', reject);
    if (body) r.write(body);
    r.end();
  });
}

test('device flow: subscribe, poll, goal wakes the long poll', async () => {
  await new Promise((r) => server.listen(0, r));
  const port = server.address().port;
  try {
    const sub = {
      name: 'desk', loc: { lat: 12.97, lon: 77.59 }, tz: 'Asia/Kolkata', wx: 1, aq: 1, qt: 1, nw: 'bbc-world',
      fb: { lg: ['eng.1'], tm: [{ id: '359', lg: 'eng.1' }], tb: ['eng.1'] },
      cr: { tm: [{ id: '6' }] },
    };
    const r1 = await req(port, '/v1/sub', { method: 'POST', body: sub });
    assert.equal(r1.status, 200);
    const id = r1.body.id;

    const f1 = await req(port, `/v1/feed?sub=${id}&v=0`);
    assert.equal(f1.status, 200);
    const f = f1.body;
    assert.equal(f.wx.t, 27.4);
    assert.equal(f.wx.dy.length, 6);
    assert.equal(f.aq.aqi, 88);
    assert.equal(f.fb[0].h.a, 'ARS');
    assert.equal(f.fb[0].m, 1);
    assert.equal(f.tb[0].r[0][1], 'ARS');
    assert.equal(f.tb[0].r[0][5], 1, 'followed team highlighted');
    assert.equal(f.cr[0].a[0].a, 'IND');
    assert.equal(f.cr[0].a[0].b, 1);
    assert.ok(f.cr.length >= 2, 'live + next match for followed team');
    assert.ok(!f.cr.some((m) => m.ti === 'NSW v VIC'), 'unfollowed live match hidden unless "all"');
    assert.equal(f.nw[0].h, 'Leaders meet for climate talks');
    assert.equal(f.qt.q, 'Stay hungry.');
    assert.deepEqual(f.ev, []);

    // Nothing changes -> 304 after the wait.
    const t0 = Date.now();
    const idle = await req(port, `/v1/feed?sub=${id}&v=${f.v}&wait=1`);
    assert.equal(idle.status, 304);
    assert.ok(Date.now() - t0 >= 900);

    // A goal arrives while the device is waiting: the poll returns early.
    const s = feed.subs.get(id);
    const pending = req(port, `/v1/feed?sub=${id}&v=${f.v}&wait=20`);
    await new Promise((r) => setTimeout(r, 200));
    goals = 1;
    require('../src/http').cache.delete('fb:today:eng.1');
    require('../src/http').cache.delete('fb:wide:eng.1');
    const t1 = Date.now();
    await feed.build(s);
    const woke = await pending;
    assert.equal(woke.status, 200);
    assert.ok(Date.now() - t1 < 2000, 'woken immediately');
    assert.equal(woke.body.fb[0].h.s, 1);
    assert.equal(woke.body.ev.length, 1);
    assert.equal(woke.body.ev[0].k, 'goal');
    assert.equal(woke.body.ev[0].ti, 'GOAL! ARS 1-0 CHE');
    assert.equal(woke.body.fb[0].gh, "B. Saka 20'");

    // Unknown subscription -> 404 so the device re-registers.
    assert.equal((await req(port, '/v1/feed?sub=nope')).status, 404);
    // Push a message from a script / Home Assistant.
    const push = await req(port, '/v1/push', { method: 'POST', body: { text: 'Dinner is ready', from: 'Mum' } });
    assert.equal(push.body.delivered >= 1, true);
    const f3 = await req(port, `/v1/feed?sub=${id}&v=${woke.body.v}`);
    assert.equal(f3.body.ev.at(-1).k, 'msg');
    assert.equal(f3.body.ev.at(-1).tx, 'Dinner is ready');
    // Payload stays small for the ESP32.
    assert.ok(JSON.stringify(f3.body).length < 9000, `feed size ${JSON.stringify(f3.body).length}`);
  } finally {
    server.close();
  }
});
