'use strict';
// Hand-made payloads in the shape ESPN / Open-Meteo return, trimmed to the
// fields the hub reads. Used by the unit and end-to-end tests.

function soccerEvent({ id = '701', state = 'in', hs = 1, as = 0, clock = "34'", detail = "34'", goals = [], date = new Date(Date.now() - 34 * 60000).toISOString() } = {}) {
  return {
    id, date, name: 'Chelsea at Arsenal', shortName: 'CHE @ ARS',
    competitions: [{
      date,
      venue: { fullName: 'Emirates Stadium' },
      status: { displayClock: clock, period: 1, type: { state, completed: state === 'post', shortDetail: detail, detail, name: state === 'post' ? 'STATUS_FULL_TIME' : 'STATUS_FIRST_HALF' } },
      competitors: [
        { homeAway: 'home', score: String(hs), winner: false, team: { id: '359', abbreviation: 'ARS', displayName: 'Arsenal', shortDisplayName: 'Arsenal' } },
        { homeAway: 'away', score: String(as), winner: false, team: { id: '363', abbreviation: 'CHE', displayName: 'Chelsea', shortDisplayName: 'Chelsea' } },
      ],
      details: goals.map((g) => ({
        type: { text: g.own ? 'Own Goal' : 'Goal' }, scoringPlay: true, ownGoal: !!g.own, penaltyKick: !!g.pen,
        clock: { displayValue: g.min }, team: { id: g.team },
        athletesInvolved: [{ displayName: g.name, shortName: g.short }],
      })),
    }],
  };
}

function cricketHeader({ state = 'in', ind = '145/4 (16.2 ov)', aus = '188/6 (20 ov)', summary = 'India need 44 runs in 22 balls' } = {}) {
  return {
    sports: [{
      leagues: [{
        id: '1430001', name: 'Australia tour of India', abbreviation: 'AUS in IND',
        events: [{
          id: '1430099', date: new Date(Date.now() - 3 * 3600000).toISOString(),
          name: 'India v Australia', shortName: 'IND v AUS', description: '3rd T20I',
          status: state, summary,
          fullStatus: { type: { state, detail: summary } },
          competitors: [
            { id: '6', abbreviation: 'IND', displayName: 'India', score: ind, homeAway: 'home', winner: false },
            { id: '2', abbreviation: 'AUS', displayName: 'Australia', score: aus, homeAway: 'away', winner: false },
          ],
        }, {
          id: '1430100', date: new Date(Date.now() + 2 * 86400000).toISOString(),
          name: 'India v Australia', shortName: 'IND v AUS', description: '4th T20I', status: 'pre', summary: 'Match yet to begin',
          competitors: [
            { id: '6', abbreviation: 'IND', displayName: 'India', score: '' },
            { id: '2', abbreviation: 'AUS', displayName: 'Australia', score: '' },
          ],
        }],
      }, {
        id: '1440000', name: 'Sheffield Shield', abbreviation: 'SS',
        events: [{
          id: '1440001', date: new Date(Date.now() - 86400000).toISOString(), name: 'NSW v VIC', shortName: 'NSW v VIC', status: 'in', summary: 'Day 2 - VIC lead by 12 runs',
          competitors: [{ id: '50', abbreviation: 'NSW', displayName: 'New South Wales', score: '312' }, { id: '51', abbreviation: 'VIC', displayName: 'Victoria', score: '324/7 (98 ov)' }],
        }],
      }],
    }],
  };
}

function cricketSeries({ indRuns = 145, indW = 4, indOv = 16.2 } = {}) {
  return {
    events: [{
      id: '1430099',
      competitions: [{
        status: { type: { state: 'in', detail: 'India need 44 runs in 22 balls' } },
        competitors: [
          { team: { id: '6', abbreviation: 'IND' }, linescores: [{ period: 2, runs: indRuns, wickets: indW, overs: indOv, isBatting: true }] },
          { team: { id: '2', abbreviation: 'AUS' }, linescores: [{ period: 1, runs: 188, wickets: 6, overs: 20, isBatting: false }] },
        ],
      }],
    }],
  };
}

const openMeteo = {
  current: { temperature_2m: 27.4, relative_humidity_2m: 61, apparent_temperature: 29.1, weather_code: 2, wind_speed_10m: 11.2, is_day: 1, precipitation: 0 },
  hourly: {
    time: Array.from({ length: 13 }, (_, i) => `2026-10-30T${String(10 + i).padStart(2, '0')}:00`),
    temperature_2m: Array.from({ length: 13 }, (_, i) => 26 + i * 0.3),
    weather_code: Array.from({ length: 13 }, () => 2),
    precipitation_probability: Array.from({ length: 13 }, () => 10),
  },
  daily: {
    time: ['2026-10-30', '2026-10-31', '2026-11-01', '2026-11-02', '2026-11-03', '2026-11-04'],
    weather_code: [2, 61, 3, 0, 1, 95], temperature_2m_max: [30, 28, 29, 31, 31, 27], temperature_2m_min: [21, 20, 20, 22, 22, 21],
    sunrise: ['2026-10-30T06:12'], sunset: ['2026-10-30T17:48'], uv_index_max: [7.5], precipitation_probability_max: [10, 70, 20, 0, 5, 80],
  },
};

const rss = `<?xml version="1.0"?><rss><channel><title>BBC</title>
<item><title><![CDATA[Leaders meet for climate talks]]></title></item>
<item><title>Scientists find &quot;new&quot; species &amp; more</title></item>
<item><title>Café owner’s big win - Local Paper</title><source url="x">Local Paper</source></item>
</channel></rss>`;

module.exports = { soccerEvent, cricketHeader, cricketSeries, openMeteo, rss };
