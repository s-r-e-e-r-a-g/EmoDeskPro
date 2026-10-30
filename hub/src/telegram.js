'use strict';
// Optional Telegram bot. Runs on the hub (not the ESP32), so the device no
// longer holds a TLS long-poll open to Telegram.
//
//   TELEGRAM_TOKEN   bot token from @BotFather (required to enable)
//   TELEGRAM_CHATS   comma-separated chat ids allowed to control Emo. When
//                    empty, the first chat that sends /start is paired and
//                    remembered in hub-data.json.
const fs = require('fs');
const path = require('path');
const { clip } = require('./util');

const DATA = process.env.HUB_DATA || path.join(__dirname, '..', 'hub-data.json');
const MOODS = ['happy', 'sad', 'angry', 'love', 'surprised', 'sleepy', 'excited', 'neutral', 'wink', 'dizzy'];

function load() { try { return JSON.parse(fs.readFileSync(DATA, 'utf8')); } catch (_) { return {}; } }
function save(d) { try { fs.writeFileSync(DATA, JSON.stringify(d, null, 2)); } catch (e) { console.error('[tg] save', e.message); } }

function start({ broadcast, status }) {
  const token = process.env.TELEGRAM_TOKEN;
  if (!token) return null;
  const api = (m, body) => fetch(`https://api.telegram.org/bot${token}/${m}`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body || {}),
    signal: AbortSignal.timeout(40000),
  }).then((r) => r.json());
  const data = load();
  const allowed = new Set((process.env.TELEGRAM_CHATS || '').split(',').map((x) => x.trim()).filter(Boolean));
  if (data.tgChat) allowed.add(String(data.tgChat));
  let offset = 0;
  let stopped = false;
  const reply = (chat, text) => api('sendMessage', { chat_id: chat, text }).catch(() => {});
  const HELP = 'Send any text to show it on Emo.\n/emo happy|sad|love|angry|surprised|sleepy|excited\n/next  /prev  /home\n/status';

  async function handle(msg) {
    const chat = String(msg.chat.id);
    const text = (msg.text || '').trim();
    if (!text) return;
    if (!allowed.size && text.startsWith('/start')) {
      allowed.add(chat);
      data.tgChat = chat;
      save(data);
      return reply(chat, `Paired with EmoDesk.\n${HELP}`);
    }
    if (!allowed.has(chat)) return reply(chat, 'This bot is private. Ask the owner to add your chat id: ' + chat);
    const from = clip(msg.from && (msg.from.first_name || msg.from.username) || 'Telegram', 16);
    const [cmd, ...rest] = text.split(/\s+/);
    const arg = rest.join(' ');
    const c = cmd.toLowerCase().replace(/@.*$/, '');
    if (c === '/start' || c === '/help') return reply(chat, HELP);
    if (c === '/status') return reply(chat, status());
    if (c === '/emo') {
      const mood = arg.toLowerCase();
      if (!MOODS.includes(mood)) return reply(chat, `Moods: ${MOODS.join(', ')}`);
      broadcast({ k: 'cmd', c: `emo:${mood}`, ti: '', tx: '' });
      return reply(chat, `Emo is ${mood} now.`);
    }
    if (['/next', '/prev', '/home'].includes(c)) {
      broadcast({ k: 'cmd', c: c.slice(1), ti: '', tx: '' });
      return reply(chat, 'Done.');
    }
    const body = c === '/say' ? arg : text;
    if (!body) return;
    const n = broadcast({ k: 'msg', ti: from, tx: clip(body, 200) });
    return reply(chat, n ? 'Shown on Emo.' : 'No EmoDesk is connected to the hub right now.');
  }

  (async function loop() {
    console.log('[tg] bot enabled');
    while (!stopped) {
      try {
        const r = await api('getUpdates', { offset, timeout: 30, allowed_updates: ['message'] });
        for (const u of r.result || []) {
          offset = u.update_id + 1;
          if (u.message) await handle(u.message).catch((e) => console.error('[tg]', e.message));
        }
        if (!r.ok) await new Promise((x) => setTimeout(x, 10000));
      } catch (e) {
        await new Promise((x) => setTimeout(x, 5000));
      }
    }
  })();
  return { stop: () => { stopped = true; } };
}

module.exports = { start };
