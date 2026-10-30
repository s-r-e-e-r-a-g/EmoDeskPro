'use strict';
// Headlines from RSS. Built-in sources or any RSS/Atom URL, or "q:<search>"
// for a Google News search.
const { getText, cached } = require('./http');
const { clip, decodeEntities } = require('./util');

const SOURCES = {
  'bbc-top': ['BBC', 'https://feeds.bbci.co.uk/news/rss.xml'],
  'bbc-world': ['BBC', 'https://feeds.bbci.co.uk/news/world/rss.xml'],
  'bbc-tech': ['BBC', 'https://feeds.bbci.co.uk/news/technology/rss.xml'],
  'bbc-sport': ['BBC', 'https://feeds.bbci.co.uk/sport/rss.xml'],
  'bbc-business': ['BBC', 'https://feeds.bbci.co.uk/news/business/rss.xml'],
  'bbc-science': ['BBC', 'https://feeds.bbci.co.uk/news/science_and_environment/rss.xml'],
  'guardian-world': ['Guardian', 'https://www.theguardian.com/world/rss'],
  'hn': ['HN', 'https://hnrss.org/frontpage'],
  'verge': ['Verge', 'https://www.theverge.com/rss/index.xml'],
  'hindu': ['Hindu', 'https://www.thehindu.com/news/national/feeder/default.rss'],
  'cricinfo': ['Cricinfo', 'https://www.espncricinfo.com/rss/content/story/feeds/0.xml'],
};

function parseFeed(xml) {
  const items = [];
  const re = /<(item|entry)\b[\s\S]*?<\/\1>/gi;
  let m;
  while ((m = re.exec(xml)) && items.length < 10) {
    const t = m[0].match(/<title\b[^>]*>([\s\S]*?)<\/title>/i);
    if (!t) continue;
    let title = decodeEntities(t[1]).replace(/<[^>]+>/g, '').trim();
    // Google News appends " - Publisher"
    let src = '';
    const s = m[0].match(/<source\b[^>]*>([\s\S]*?)<\/source>/i);
    if (s) {
      src = decodeEntities(s[1]).trim();
      if (title.endsWith(` - ${src}`)) title = title.slice(0, -(src.length + 3));
    }
    if (title) items.push({ h: clip(title, 110), s: clip(src, 14) });
  }
  return items;
}

async function headlines(source) {
  source = String(source || 'bbc-world').trim();
  let label = 'News', url;
  if (SOURCES[source]) [label, url] = SOURCES[source];
  else if (source.startsWith('q:')) {
    url = `https://news.google.com/rss/search?q=${encodeURIComponent(source.slice(2))}&hl=en&gl=US&ceid=US:en`;
    label = '';
  } else if (/^https?:\/\//.test(source)) url = source;
  else [label, url] = SOURCES['bbc-world'];
  return cached(`nw:${url}`, 15 * 60000, async () => {
    const items = parseFeed(await getText(url, { timeout: 10000 }));
    return items.slice(0, 8).map((i) => ({ h: i.h, s: i.s || label }));
  });
}

module.exports = { SOURCES, parseFeed, headlines };
