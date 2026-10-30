'use strict';
// Quote of the hour. Uses ZenQuotes when reachable, otherwise a built-in list.
const { getJson, cached } = require('./http');
const { clip } = require('./util');

const LOCAL = [
  ['Well begun is half done.', 'Aristotle'],
  ['The secret of getting ahead is getting started.', 'Mark Twain'],
  ['It does not matter how slowly you go as long as you do not stop.', 'Confucius'],
  ['Simplicity is the ultimate sophistication.', 'Leonardo da Vinci'],
  ['Knowing is not enough; we must apply.', 'Goethe'],
  ['What we think, we become.', 'Buddha'],
  ['Energy and persistence conquer all things.', 'Benjamin Franklin'],
  ['The best way out is always through.', 'Robert Frost'],
  ['Nothing will work unless you do.', 'Maya Angelou'],
  ['Be so good they cannot ignore you.', 'Steve Martin'],
  ['Make it work, make it right, make it fast.', 'Kent Beck'],
  ['Little by little, one travels far.', 'J.R.R. Tolkien'],
  ['Action is the foundational key to all success.', 'Pablo Picasso'],
  ['The journey of a thousand miles begins with one step.', 'Lao Tzu'],
  ['Quality is not an act, it is a habit.', 'Aristotle'],
  ['Arise, awake, and stop not till the goal is reached.', 'Swami Vivekananda'],
];

function local() {
  const [q, a] = LOCAL[Math.floor(Date.now() / 3600000) % LOCAL.length];
  return { q, a };
}

async function quote() {
  return cached('qt', 60 * 60000, async () => {
    try {
      const j = await getJson('https://zenquotes.io/api/random', { timeout: 6000 });
      const x = Array.isArray(j) ? j[0] : null;
      if (x && x.q && x.q.length < 140 && !/too many requests/i.test(x.q)) return { q: clip(x.q, 140), a: clip(x.a, 24) };
    } catch (_) { /* fall through */ }
    return local();
  });
}

module.exports = { quote, local };
