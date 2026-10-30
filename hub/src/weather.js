'use strict';
// Weather + air quality from Open-Meteo (no key).
const { getJson, cached } = require('./http');
const { int, round1 } = require('./util');

const key = (lat, lon) => `${(+lat).toFixed(2)},${(+lon).toFixed(2)}`;

async function weather(lat, lon, imperial) {
  return cached(`wx:${key(lat, lon)}:${imperial ? 'f' : 'c'}`, 10 * 60000, async () => {
    const u = new URL('https://api.open-meteo.com/v1/forecast');
    u.search = new URLSearchParams({
      latitude: (+lat).toFixed(4), longitude: (+lon).toFixed(4),
      current: 'temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,is_day,precipitation',
      hourly: 'temperature_2m,weather_code,precipitation_probability',
      daily: 'weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max,precipitation_probability_max',
      timezone: 'auto', forecast_days: '6', forecast_hours: '13',
      ...(imperial ? { temperature_unit: 'fahrenheit', wind_speed_unit: 'mph', precipitation_unit: 'inch' } : {}),
    }).toString();
    const j = await getJson(u.toString());
    const c = j.current || {};
    const h = j.hourly || {};
    const d = j.daily || {};
    const hm = (s) => (typeof s === 'string' && s.length >= 16 ? s.slice(11, 16) : '');
    const hours = [];
    for (let i = 1; i < (h.time || []).length && hours.length < 12; i++) {
      hours.push([int(h.time[i].slice(11, 13)), round1(h.temperature_2m[i]), int(h.weather_code[i]), int((h.precipitation_probability || [])[i])]);
    }
    const days = [];
    for (let i = 0; i < (d.time || []).length && days.length < 6; i++) {
      const wd = new Date(`${d.time[i]}T12:00:00Z`).getUTCDay();
      days.push([wd, int(d.weather_code[i]), round1(d.temperature_2m_max[i]), round1(d.temperature_2m_min[i]), int((d.precipitation_probability_max || [])[i])]);
    }
    return {
      t: round1(c.temperature_2m), f: round1(c.apparent_temperature), h: int(c.relative_humidity_2m),
      w: round1(c.wind_speed_10m), c: int(c.weather_code), d: c.is_day === 0 ? 0 : 1, p: round1(c.precipitation),
      hi: days[0] ? days[0][2] : 0, lo: days[0] ? days[0][3] : 0,
      sr: hm((d.sunrise || [])[0]), ss: hm((d.sunset || [])[0]), uv: round1((d.uv_index_max || [])[0]),
      hr: hours, dy: days,
    };
  });
}

async function air(lat, lon) {
  return cached(`aq:${key(lat, lon)}`, 30 * 60000, async () => {
    const u = `https://air-quality-api.open-meteo.com/v1/air-quality?latitude=${(+lat).toFixed(4)}&longitude=${(+lon).toFixed(4)}&current=us_aqi,pm2_5,pm10,uv_index`;
    const c = (await getJson(u)).current || {};
    return { aqi: int(c.us_aqi), pm25: round1(c.pm2_5), pm10: round1(c.pm10), uv: round1(c.uv_index) };
  });
}

async function geocode(q) {
  return cached(`geo:${q.toLowerCase()}`, 7 * 86400000, async () => {
    const j = await getJson(`https://geocoding-api.open-meteo.com/v1/search?count=6&language=en&name=${encodeURIComponent(q)}`);
    return (j.results || []).map((r) => ({
      name: r.name, region: [r.admin1, r.country].filter(Boolean).join(', '),
      lat: r.latitude, lon: r.longitude, tz: r.timezone || '',
    }));
  });
}

module.exports = { weather, air, geocode };
