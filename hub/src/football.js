'use strict';
// Football (soccer) from ESPN's public site API.
//
//   scoreboard: site.api.espn.com/apis/site/v2/sports/soccer/{lg}/scoreboard[?dates=A-B]
//   standings:  site.api.espn.com/apis/v2/sports/soccer/{lg}/standings
//   teams:      site.api.espn.com/apis/site/v2/sports/soccer/{lg}/teams
//
// Teams are matched by ESPN team id (picked from a list in the dashboard), not
// by fuzzy name matching, so "India" never matches "India Women" and
// "Manchester" never matches both Manchester clubs.
const { getJson, cached, peek } = require('./http');
const { clip, int, unix, ymd } = require('./util');

const BASE = 'https://site.api.espn.com/apis/site/v2/sports/soccer';
const BASE_V2 = 'https://site.api.espn.com/apis/v2/sports/soccer';

const LEAGUES = [
  ['eng.1', 'Premier League', 'EPL'], ['esp.1', 'LaLiga', 'LIGA'], ['ita.1', 'Serie A', 'SERA'],
  ['ger.1', 'Bundesliga', 'BUN'], ['fra.1', 'Ligue 1', 'L1'], ['uefa.champions', 'Champions League', 'UCL'],
  ['uefa.europa', 'Europa League', 'UEL'], ['uefa.europa.conf', 'Conference League', 'UECL'],
  ['eng.2', 'Championship', 'EFL'], ['eng.fa', 'FA Cup', 'FAC'], ['eng.league_cup', 'League Cup', 'EFLC'],
  ['ned.1', 'Eredivisie', 'ERE'], ['por.1', 'Primeira Liga', 'POR'], ['sco.1', 'Scottish Prem', 'SPFL'],
  ['usa.1', 'MLS', 'MLS'], ['mex.1', 'Liga MX', 'LMX'], ['bra.1', 'Brasileirao', 'BRA'],
  ['arg.1', 'Liga Profesional', 'ARG'], ['sau.1', 'Saudi Pro League', 'SPL'], ['ind.1', 'Indian Super League', 'ISL'],
  ['fifa.world', 'World Cup', 'WC'], ['uefa.euro', 'Euro', 'EURO'], ['conmebol.america', 'Copa America', 'COPA'],
  ['fifa.worldq.uefa', 'WC Qual. UEFA', 'WCQ'], ['uefa.nations', 'Nations League', 'UNL'],
  ['fifa.friendly', 'Friendlies', 'FRI'],
];
const LG = Object.fromEntries(LEAGUES.map(([id, name, abbr]) => [id, { id, name, abbr }]));
const validLeague = (lg) => /^[a-z0-9_.]{2,24}$/.test(lg);
const lgAbbr = (lg) => (LG[lg] ? LG[lg].abbr : lg.toUpperCase().slice(0, 5));

function athleteShort(a) {
  if (!a) return '';
  if (a.shortName) return a.shortName;
  const d = a.displayName || a.fullName || '';
  const parts = d.split(' ');
  return parts.length > 1 ? parts.slice(1).join(' ') : d;
}

// Normalise one ESPN event into the compact shape the rest of the hub uses.
function parseEvent(ev, lg) {
  const comp = (ev.competitions || [])[0] || {};
  const st = (comp.status || ev.status || {});
  const type = st.type || {};
  const cs = comp.competitors || [];
  const home = cs.find((c) => c.homeAway === 'home') || cs[0] || {};
  const away = cs.find((c) => c.homeAway === 'away') || cs[1] || {};
  const side = (c) => {
    const t = c.team || {};
    return {
      id: String(t.id || c.id || ''),
      a: clip(t.abbreviation || t.shortDisplayName || t.displayName || '?', 4).toUpperCase(),
      n: clip(t.shortDisplayName || t.displayName || t.name || '', 18),
      sc: c.score === undefined || c.score === '' ? null : int(typeof c.score === 'object' ? c.score.value : c.score),
      win: !!c.winner,
    };
  };
  const h = side(home), a = side(away);
  const goals = [];
  let redH = 0, redA = 0;
  for (const d of comp.details || []) {
    const teamId = String((d.team || {}).id || '');
    const who = athleteShort((d.athletesInvolved || [])[0]);
    const min = ((d.clock || {}).displayValue || '').replace(/\s/g, '');
    const text = ((d.type || {}).text || '').toLowerCase();
    if (d.scoringPlay) {
      // Own goals count for the other team but ESPN tags them with the scorer's team.
      const own = !!d.ownGoal || text.includes('own goal');
      let isHome = teamId === h.id;
      if (own) isHome = !isHome;
      goals.push({ s: isHome ? 'h' : 'a', who: clip(who + (own ? ' (og)' : '') + (d.penaltyKick ? ' (p)' : ''), 16), min });
    } else if (d.redCard || text.includes('red card')) {
      if (teamId === h.id) redH++; else if (teamId === a.id) redA++;
    }
  }
  const state = type.state || 'pre';
  return {
    id: String(ev.id),
    lg,
    state, // pre | in | post
    done: !!type.completed,
    detail: clip(type.shortDetail || type.detail || type.description || '', 14),
    clock: st.displayClock || '',
    period: int(st.period),
    start: unix(ev.date || comp.date),
    h, a, goals, redH, redA,
    venue: clip(((comp.venue || {}).fullName) || '', 24),
    postponed: /postponed|canceled|cancelled|abandoned|suspended/i.test(type.name || type.description || ''),
  };
}

async function scoreboard(lg, range) {
  const url = range ? `${BASE}/${lg}/scoreboard?dates=${range}&limit=200` : `${BASE}/${lg}/scoreboard`;
  const j = await getJson(url);
  return (j.events || []).map((e) => parseEvent(e, lg));
}

// Two-tier polling per league: a wide window (last 3 days .. next 10 days)
// refreshed slowly gives "last result / next fixture", and today's scoreboard
// is refreshed every 15 s while anything is live or about to kick off.
const liveMemo = new Map(); // lg -> bool (was anything live/imminent last time)

async function league(lg) {
  if (!validLeague(lg)) return [];
  const range = `${ymd(-3)}-${ymd(10)}`;
  const wide = await cached(`fb:wide:${lg}`, 15 * 60000, () => scoreboard(lg, range)).catch(() => []);
  const hot = liveMemo.get(lg);
  const today = await cached(`fb:today:${lg}`, hot ? 15000 : 5 * 60000, () => scoreboard(lg)).catch(() => []);
  const byId = new Map();
  for (const e of wide) byId.set(e.id, e);
  for (const e of today) byId.set(e.id, e); // fresher wins
  const all = [...byId.values()].sort((x, y) => x.start - y.start);
  const now = Date.now() / 1000;
  liveMemo.set(lg, all.some((e) => e.state === 'in' || (e.state === 'pre' && e.start - now < 900 && e.start - now > -3600)));
  return all;
}

const isHot = (lg) => !!liveMemo.get(lg);

async function standings(lg) {
  if (!validLeague(lg)) return null;
  return cached(`fb:table:${lg}`, isHot(lg) ? 5 * 60000 : 30 * 60000, async () => {
    const j = await getJson(`${BASE_V2}/${lg}/standings`);
    const groups = j.children && j.children.length ? j.children : [j];
    const g = groups[0] || {};
    const entries = ((g.standings || {}).entries || []).map((en) => {
      const s = {};
      for (const x of en.stats || []) s[x.name || x.type] = x.value;
      const t = en.team || {};
      return {
        id: String(t.id || ''),
        a: clip(t.abbreviation || t.shortDisplayName || '?', 4).toUpperCase(),
        rank: int(s.rank, 0),
        pts: int(s.points),
        gp: int(s.gamesPlayed),
        gd: int(s.pointDifferential),
        w: int(s.wins), d: int(s.ties), l: int(s.losses),
      };
    });
    entries.sort((x, y) => (x.rank || 99) - (y.rank || 99) || y.pts - x.pts || y.gd - x.gd);
    entries.forEach((e, i) => { if (!e.rank) e.rank = i + 1; });
    return { lg, name: clip((g.name || (LG[lg] || {}).name || lg), 22), rows: entries.slice(0, 24) };
  });
}

async function teams(lg) {
  if (!validLeague(lg)) return [];
  return cached(`fb:teams:${lg}`, 24 * 3600000, async () => {
    const j = await getJson(`${BASE}/${lg}/teams`);
    const list = ((((j.sports || [])[0] || {}).leagues || [])[0] || {}).teams || [];
    return list.map(({ team: t = {} }) => ({ id: String(t.id), abbr: t.abbreviation || '', name: t.displayName || t.name || '' }))
      .sort((x, y) => x.name.localeCompare(y.name));
  });
}

module.exports = { LEAGUES, LG, lgAbbr, validLeague, parseEvent, league, standings, teams, isHot, peek };
