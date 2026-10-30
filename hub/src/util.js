'use strict';
// Small helpers shared by every source module.

const UA = 'Mozilla/5.0 (EmoDeskHub/2.0; +https://github.com/sreeraganil/EmoDesk)';

// The OLED fonts are ASCII only, so every string that reaches the device is
// folded to printable ASCII here instead of on the microcontroller.
const MAP = {
  '‘': "'", '’': "'", '‚': "'", '‛': "'", '′': "'",
  '“': '"', '”': '"', '„': '"', '″': '"',
  '–': '-', '—': '-', '−': '-', '…': '...', ' ': ' ',
  'ß': 'ss', 'æ': 'ae', 'Æ': 'AE', 'ø': 'o', 'Ø': 'O',
  'ł': 'l', 'Ł': 'L', 'đ': 'd', 'Đ': 'D', 'þ': 'th',
  '€': 'EUR', '£': 'GBP', '₹': 'Rs', '¥': 'JPY', '°': ' deg',
};

function ascii(s) {
  if (s === null || s === undefined) return '';
  let out = String(s).normalize('NFD').replace(/[̀-ͯ]/g, '');
  out = out.replace(/[^\x20-\x7e]/g, (c) => (MAP[c] !== undefined ? MAP[c] : ''));
  return out.replace(/\s+/g, ' ').trim();
}

// Cut to n characters on a word boundary when possible.
function clip(s, n) {
  s = ascii(s);
  if (s.length <= n) return s;
  const cut = s.slice(0, n - 1);
  const sp = cut.lastIndexOf(' ');
  return (sp >= n * 0.5 ? cut.slice(0, sp) : cut).replace(/[\s,.;:-]+$/, '') + '~';
}

function decodeEntities(s) {
  return String(s || '')
    .replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, '$1')
    .replace(/&#(\d+);/g, (_, d) => String.fromCodePoint(+d))
    .replace(/&#x([0-9a-f]+);/gi, (_, h) => String.fromCodePoint(parseInt(h, 16)))
    .replace(/&quot;/g, '"').replace(/&apos;/g, "'").replace(/&#39;/g, "'")
    .replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&nbsp;/g, ' ')
    .replace(/&amp;/g, '&');
}

const num = (v, d = 0) => {
  const n = typeof v === 'number' ? v : parseFloat(v);
  return Number.isFinite(n) ? n : d;
};
const int = (v, d = 0) => Math.round(num(v, d));
const round1 = (v) => Math.round(num(v) * 10) / 10;
const unix = (d) => {
  const t = Date.parse(d);
  return Number.isFinite(t) ? Math.floor(t / 1000) : 0;
};

// YYYYMMDD in UTC, offset by `days`.
function ymd(days = 0, now = Date.now()) {
  const d = new Date(now + days * 86400000);
  return d.getUTCFullYear() * 10000 + (d.getUTCMonth() + 1) * 100 + d.getUTCDate();
}

// Squash a series into `n` integers 0..15 for the device sparkline.
function spark(values, n = 24) {
  const v = (values || []).map(Number).filter(Number.isFinite);
  if (v.length < 2) return [];
  const out = [];
  for (let i = 0; i < n; i++) out.push(v[Math.min(v.length - 1, Math.round((i * (v.length - 1)) / (n - 1)))]);
  const lo = Math.min(...out), hi = Math.max(...out);
  const span = hi - lo || 1;
  return out.map((x) => Math.round(((x - lo) / span) * 15));
}

// 67321.4 -> "67,321"   0.0001234 -> "0.000123"
function fmtPrice(p) {
  p = num(p);
  const a = Math.abs(p);
  let dec = 2;
  if (a >= 1000) dec = 0;
  else if (a >= 1) dec = 2;
  else if (a >= 0.01) dec = 4;
  else dec = 6;
  return p.toLocaleString('en-US', { minimumFractionDigits: dec > 2 ? 2 : dec, maximumFractionDigits: dec });
}

module.exports = { UA, ascii, clip, decodeEntities, num, int, round1, unix, ymd, spark, fmtPrice };
