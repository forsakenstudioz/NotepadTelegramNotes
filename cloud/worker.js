// Telegram Notes cloud bot - a Cloudflare Worker that answers your Telegram bot 24/7,
// even while your PC is off. The Notepad++ plugin uploads your notes here and
// downloads the notes you type on your phone.
//
// Needs one KV namespace binding named NOTES. Everything else is set up from
// Notepad++ (Plugins > Telegram Notes > Connect to cloud).
//
// KV keys:
//   config        {syncKey, botToken, chatId, hookSecret, bot}
//   note:<name>   note text (metadata: {size, updated})
//   inbox         [{id, date, text}] notes typed on the phone, waiting for Notepad++
//   list:<name>   [{done, text}] todo / grocery lists (mirrored to todo.txt / grocery.txt on the PC)
//   lastlist      names shown by the last /list, so "/get 2" works

const TG = "https://api.telegram.org";
const LONG_NOTE = 3800;

const LISTS = [
  { cmd: "todo", tick: "done", title: "Todo", emoji: "📝", commas: false },
  { cmd: "grocery", tick: "got", title: "Grocery list", emoji: "🛒", commas: true },
];

const COMMANDS = [
  { command: "list", description: "Show your notes" },
  { command: "get", description: "Send me a note: /get name or /get 2" },
  { command: "todo", description: "Show todo list, or add: /todo buy milk" },
  { command: "done", description: "Tick a todo item: /done 2" },
  { command: "grocery", description: "Show grocery list, or add: /grocery eggs, bread" },
  { command: "got", description: "Tick a grocery item: /got 1" },
  { command: "clear", description: "Remove ticked items: /clear todo or /clear grocery" },
  { command: "del", description: "Delete a note from the cloud: /del name" },
  { command: "ping", description: "Check the bot is online" },
  { command: "help", description: "How to use this bot" },
];

const HELP =
  "👋 <b>Notepad++ Telegram Notes</b>\n" +
  "I live in the cloud, so I answer even when your PC is off.\n\n" +
  "<b>Type any message</b> and I keep it for Notepad++. It opens there with " +
  "Plugins › Telegram Notes › Fetch notes from phone.\n\n" +
  "/list – show your notes\n" +
  "/get name – send me a note (or /get 2)\n" +
  "/todo – todo list · /todo buy milk – add\n" +
  "/done 2 – tick todo item 2\n" +
  "/grocery – grocery list · /grocery eggs, bread – add\n" +
  "/got 1 – tick grocery item 1\n" +
  "/clear todo – remove ticked items (or /clear grocery)\n" +
  "/del name – delete a note from the cloud\n" +
  "/ping – check I'm online\n\n" +
  "Notes are the last version Notepad++ uploaded (it uploads when you save).";

export default {
  async fetch(request, env) {
    try {
      return await route(request, env);
    } catch (e) {
      return json({ ok: false, error: String((e && e.message) || e) }, 500);
    }
  },
};

async function route(request, env) {
  const url = new URL(request.url);
  if (!env.NOTES)
    return text("Almost there: add a KV namespace binding named NOTES to this Worker (Settings › Bindings), then deploy again.", 500);
  if (url.pathname === "/telegram" && request.method === "POST") return telegram(request, env);
  if (url.pathname.startsWith("/api/")) return api(request, env, url);
  const cfg = await getConfig(env);
  return text(
    cfg
      ? `Telegram Notes cloud is running and connected to @${cfg.bot}.`
      : "Telegram Notes cloud is running. Now open Notepad++ and click Plugins › Telegram Notes › Connect to cloud."
  );
}

// ---------------------------------------------------------------------------
// API used by the Notepad++ plugin
// ---------------------------------------------------------------------------
async function api(request, env, url) {
  const cfg = await getConfig(env);
  const path = url.pathname.slice("/api/".length);

  if (path === "setup" && request.method === "POST") {
    const body = await request.json();
    if (!body.syncKey || String(body.syncKey).length < 20) return json({ ok: false, error: "Missing sync key." }, 400);
    if (!body.botToken || !body.chatId) return json({ ok: false, error: "Missing bot token or chat id." }, 400);
    if (cfg && cfg.syncKey !== body.syncKey)
      return json({
        ok: false,
        error:
          "This cloud bot is already connected to another Notepad++ (different SyncKey). " +
          "If that was you, copy SyncKey from your old settings file. Otherwise delete the 'config' entry in the NOTES KV namespace and try again.",
      }, 403);
    const me = await tg(body.botToken, "getMe", {});
    if (!me.ok) return json({ ok: false, error: "Telegram did not accept the bot token: " + me.description }, 400);
    const newCfg = {
      syncKey: String(body.syncKey),
      botToken: String(body.botToken),
      chatId: String(body.chatId),
      hookSecret: (cfg && cfg.hookSecret) || randomHex(24),
      bot: me.result.username,
    };
    await env.NOTES.put("config", JSON.stringify(newCfg));
    const hook = await tg(newCfg.botToken, "setWebhook", {
      url: url.origin + "/telegram",
      secret_token: newCfg.hookSecret,
      allowed_updates: ["message"],
    });
    if (!hook.ok) return json({ ok: false, error: "Telegram setWebhook failed: " + hook.description }, 502);
    await tg(newCfg.botToken, "setMyCommands", { commands: COMMANDS });
    return json({ ok: true, bot: newCfg.bot });
  }

  const auth = request.headers.get("Authorization") || "";
  if (!cfg || auth !== "Bearer " + cfg.syncKey) return json({ ok: false, error: "Wrong or missing SyncKey." }, 401);

  if (path === "notes") {
    const name = cleanName(url.searchParams.get("name"));
    if (request.method === "GET" && !name) return json({ ok: true, notes: (await listNotes(env)).map((n) => n.name) });
    if (!name) return json({ ok: false, error: "Missing note name." }, 400);
    if (request.method === "PUT") {
      const body = await request.text();
      await env.NOTES.put("note:" + name, body, { metadata: { size: body.length, updated: Date.now() } });
      return json({ ok: true });
    }
    if (request.method === "DELETE") {
      await env.NOTES.delete("note:" + name);
      return json({ ok: true });
    }
  }

  if (path === "lists" && request.method === "GET") {
    const out = { ok: true };
    for (const l of LISTS) out[l.cmd] = listText((await env.NOTES.get("list:" + l.cmd, "json")) || []);
    return json(out);
  }

  if (path === "lists" && request.method === "PUT") {
    const l = LISTS.find((d) => d.cmd === url.searchParams.get("name"));
    if (!l) return json({ ok: false, error: "Unknown list." }, 400);
    const items = [];
    for (const raw of (await request.text()).split("\n")) {
      const p = stripPrefix(raw.trim());
      const t = p.text.trim();
      if (t && t !== "-" && t !== "*") items.push({ done: p.done, text: t });
    }
    await env.NOTES.put("list:" + l.cmd, JSON.stringify(items));
    return json({ ok: true, text: listText(items) });
  }

  if (path === "inbox" && request.method === "GET") {
    return json({ ok: true, items: (await env.NOTES.get("inbox", "json")) || [] });
  }

  if (path === "inbox/ack" && request.method === "POST") {
    const { upto } = await request.json();
    const items = (await env.NOTES.get("inbox", "json")) || [];
    const left = items.filter((i) => i.id > upto);
    if (left.length !== items.length) await env.NOTES.put("inbox", JSON.stringify(left));
    return json({ ok: true, left: left.length });
  }

  if (path === "status") {
    const items = (await env.NOTES.get("inbox", "json")) || [];
    return json({ ok: true, bot: cfg.bot, inbox: items.length });
  }

  return json({ ok: false, error: "Unknown API call." }, 404);
}

// ---------------------------------------------------------------------------
// Telegram webhook
// ---------------------------------------------------------------------------
async function telegram(request, env) {
  const cfg = await getConfig(env);
  if (!cfg || request.headers.get("X-Telegram-Bot-Api-Secret-Token") !== cfg.hookSecret)
    return new Response("forbidden", { status: 403 });
  const update = await request.json().catch(() => null);
  const msg = update && update.message;
  // Only you: messages from any other chat are ignored completely.
  if (!msg || String(msg.chat && msg.chat.id) !== cfg.chatId) return new Response("ok");
  try {
    await handleMessage(env, cfg, msg);
  } catch (e) {
    await reply(cfg, "⚠️ Something went wrong: " + esc(String((e && e.message) || e)));
  }
  return new Response("ok"); // always 200, so Telegram doesn't resend
}

async function handleMessage(env, cfg, msg) {
  const body = msg.text || msg.caption || "";
  if (!body) return reply(cfg, "Sorry, I can only keep text messages for now.");
  if (body[0] !== "/") return saveToInbox(env, cfg, body, msg.date);

  const m = body.match(/^\/(\S+)\s*([\s\S]*)$/);
  const cmd = m[1].toLowerCase().split("@")[0];
  const arg = m[2].trim();

  if (cmd === "start" || cmd === "help") return reply(cfg, HELP);
  if (cmd === "ping") return reply(cfg, "🟢 I'm online (in the cloud).");
  if (cmd === "list" || cmd === "notes") return cmdList(env, cfg);
  if (cmd === "get") return cmdGet(env, cfg, arg);
  if (cmd === "del" || cmd === "delete") return cmdDel(env, cfg, arg);
  if (cmd === "note") {
    if (!arg) return reply(cfg, "Just type your note as a normal message, I'll keep it for Notepad++.");
    return saveToInbox(env, cfg, arg, msg.date);
  }
  if (cmd === "clear") {
    const l = LISTS.find((d) => arg.toLowerCase().startsWith(d.cmd)) || LISTS[0];
    return listOp(env, cfg, l, "clear", "");
  }
  for (const l of LISTS) {
    if (cmd === l.cmd) return listOp(env, cfg, l, arg ? "add" : "show", arg);
    if (cmd === l.tick) return listOp(env, cfg, l, "tick", arg);
  }
  return reply(cfg, `I don't know <code>/${esc(cmd)}</code>. Send /help to see what I can do.`);
}

async function saveToInbox(env, cfg, body, date) {
  const items = (await env.NOTES.get("inbox", "json")) || [];
  const id = items.length ? items[items.length - 1].id + 1 : Date.now();
  items.push({ id, date: date || Math.floor(Date.now() / 1000), text: body });
  while (items.length > 500) items.shift();
  await env.NOTES.put("inbox", JSON.stringify(items));
  return reply(cfg, `✅ Saved (${items.length} waiting). It opens in Notepad++ with Plugins › Telegram Notes › Fetch notes from phone.`);
}

// ---------------------------------------------------------------------------
// Notes
// ---------------------------------------------------------------------------
async function listNotes(env) {
  const notes = [];
  let cursor;
  do {
    const page = await env.NOTES.list({ prefix: "note:", cursor });
    for (const k of page.keys) notes.push({ name: k.name.slice(5), ...(k.metadata || {}) });
    cursor = page.list_complete ? null : page.cursor;
  } while (cursor);
  notes.sort((a, b) => a.name.localeCompare(b.name, undefined, { sensitivity: "base" }));
  return notes;
}

async function cmdList(env, cfg) {
  const notes = await listNotes(env);
  await env.NOTES.put("lastlist", JSON.stringify(notes.map((n) => n.name)));
  if (!notes.length)
    return reply(cfg, "No notes in the cloud yet. Save a file in your notes folder in Notepad++, or use Plugins › Telegram Notes › Add this file to cloud notes.");
  let h = "📚 <b>Your notes</b>\n\n";
  notes.forEach((n, i) => {
    h += `${i + 1}. ${esc(n.name)}${n.updated ? " <i>· " + ago(n.updated) + "</i>" : ""}\n`;
  });
  return reply(cfg, h + "\nSend <code>/get 1</code> or <code>/get name</code>");
}

async function findNote(env, cfg, arg) {
  if (/^\d+$/.test(arg)) {
    const last = (await env.NOTES.get("lastlist", "json")) || (await listNotes(env)).map((n) => n.name);
    return { name: last[Number(arg) - 1] };
  }
  const notes = await listNotes(env);
  const want = arg.toLowerCase();
  const stem = (n) => n.toLowerCase().replace(/\.[^.]*$/, "");
  const exact = notes.find((n) => n.name.toLowerCase() === want || stem(n.name) === want);
  if (exact) return { name: exact.name };
  const partial = notes.filter((n) => n.name.toLowerCase().includes(want));
  if (partial.length === 1) return { name: partial[0].name };
  return { many: partial.map((n) => n.name) };
}

async function cmdGet(env, cfg, arg) {
  if (!arg) return reply(cfg, "Which note? Send /list first, then e.g. <code>/get 2</code> or <code>/get shopping</code>.");
  const found = await findNote(env, cfg, arg);
  if (found.many && found.many.length) {
    await env.NOTES.put("lastlist", JSON.stringify(found.many));
    return reply(cfg, "Several notes match. Which one?\n" + found.many.map((n, i) => `/get ${i + 1} – ${esc(n)}`).join("\n"));
  }
  const body = found.name ? await env.NOTES.get("note:" + found.name) : null;
  if (body === null) return reply(cfg, `I could not find <b>${esc(arg)}</b>. Send /list to see your notes.`);
  if (!body.trim()) return reply(cfg, `<b>${esc(found.name)}</b> is empty.`);
  return sendNote(cfg, found.name, body);
}

async function cmdDel(env, cfg, arg) {
  if (!arg) return reply(cfg, "Which note? e.g. <code>/del shopping</code>");
  const found = await findNote(env, cfg, arg);
  if (!found.name) return reply(cfg, `I could not find exactly one note called <b>${esc(arg)}</b>. Send /list to check the name.`);
  await env.NOTES.delete("note:" + found.name);
  return reply(cfg, `🗑 Deleted <b>${esc(found.name)}</b> from the cloud. (The file on your PC is untouched; it uploads again if you save it.)`);
}

async function sendNote(cfg, name, body) {
  const ext = (name.match(/\.([^.]+)$/) || [])[1] || "";
  const lang = LANGS[ext.toLowerCase()];
  if (/^(todo|grocery)\.txt$/i.test(name)) return reply(cfg, `📝 <b>${esc(name)}</b>\n\n` + todoHtml(body));
  if (body.length > LONG_NOTE) return sendDocument(cfg, name, body, `📄 <b>${esc(name)}</b> (long note, sent as a file)`);
  if (lang && ext !== "md" && ext !== "ini")
    return reply(cfg, `💻 <b>${esc(name)}</b>\n<pre><code class="language-${lang}">${esc(body)}</code></pre>`);
  return reply(cfg, `📄 <b>${esc(name)}</b>\n\n${esc(body.replace(/\r/g, "").trim())}`);
}

// ---------------------------------------------------------------------------
// Todo / grocery lists
// ---------------------------------------------------------------------------
function stripPrefix(s) {
  let m;
  if ((m = s.match(/^(?:[-*] )?\[[xX]\] |^[☑✔✅] /))) return { done: true, marked: true, text: s.slice(m[0].length) };
  if ((m = s.match(/^(?:[-*] )?\[ ?\] |^[☐•+*-] |^\d+[.)] /))) return { done: false, marked: true, text: s.slice(m[0].length) };
  return { done: false, marked: false, text: s };
}

function todoHtml(body) {
  let out = "";
  for (const raw of body.split("\n")) {
    const line = raw.trim();
    if (!line) continue;
    const p = stripPrefix(line);
    const item = p.text.trim();
    if (!item || item === "-" || item === "*") continue;
    if (!p.marked && item.endsWith(":")) out += (out ? "\n" : "") + `<b>${esc(item)}</b>\n`;
    else out += (p.done ? `☑ <s>${esc(item)}</s>` : `☐ ${esc(item)}`) + "\n";
  }
  return out;
}

// Same format as todo.txt / grocery.txt in the notes folder on the PC.
function listText(items) {
  return items.map((i) => (i.done ? "[x] " : "[ ] ") + i.text + "\r\n").join("");
}

function listHtml(l, items) {
  let h = `${l.emoji} <b>${l.title}</b>\n\n`;
  if (!items.length) return h + `Empty. Add something: /${l.cmd} ${l.commas ? "eggs, bread" : "buy milk"}`;
  items.forEach((it, i) => {
    h += `${i + 1}. ${it.done ? `☑ <s>${esc(it.text)}</s>` : `☐ ${esc(it.text)}`}\n`;
  });
  const open = items.filter((i) => !i.done).length;
  h += `\n${open} left. Tick one: /${l.tick} 1`;
  if (open < items.length) h += ` · Remove ticked: /clear ${l.cmd}`;
  return h;
}

async function listOp(env, cfg, l, op, arg) {
  const key = "list:" + l.cmd;
  const items = (await env.NOTES.get(key, "json")) || [];
  let note = "";
  if (op === "add") {
    let added = 0;
    for (const line of arg.split("\n")) {
      for (const part of l.commas ? line.split(",") : [line]) {
        const t = stripPrefix(part.trim()).text.trim();
        if (t) {
          items.push({ done: false, text: t });
          added++;
        }
      }
    }
    note = `➕ Added ${added}.\n\n`;
  } else if (op === "tick") {
    const nums = (arg.match(/\d+/g) || []).map(Number);
    if (!nums.length) note = `Which number? e.g. <code>/${l.tick} 2</code>\n\n`;
    for (const n of nums) {
      if (n >= 1 && n <= items.length) items[n - 1].done = !items[n - 1].done;
      else note += `There is no item ${n}.\n`;
    }
    if (note && nums.length) note += "\n";
  } else if (op === "clear") {
    const before = items.length;
    const left = items.filter((i) => !i.done);
    items.length = 0;
    items.push(...left);
    note = `🧹 Removed ${before - left.length} ticked item(s).\n\n`;
  }
  if (op !== "show") await env.NOTES.put(key, JSON.stringify(items));
  return reply(cfg, note + listHtml(l, items));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
async function getConfig(env) {
  return env.NOTES.get("config", "json");
}

async function tg(token, method, payload) {
  const r = await fetch(`${TG}/bot${token}/${method}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  return r.json().catch(() => ({ ok: false, description: "HTTP " + r.status }));
}

async function reply(cfg, html) {
  return tg(cfg.botToken, "sendMessage", {
    chat_id: cfg.chatId,
    text: html,
    parse_mode: "HTML",
    link_preview_options: { is_disabled: true },
  });
}

async function sendDocument(cfg, name, body, captionHtml) {
  const fd = new FormData();
  fd.append("chat_id", cfg.chatId);
  fd.append("caption", captionHtml);
  fd.append("parse_mode", "HTML");
  fd.append("document", new Blob([body], { type: "text/plain" }), name.includes(".") ? name : name + ".txt");
  const r = await fetch(`${TG}/bot${cfg.botToken}/sendDocument`, { method: "POST", body: fd });
  return r.json().catch(() => ({ ok: false }));
}

function cleanName(n) {
  return (n || "").replace(/[\\/]/g, "_").trim().slice(0, 200);
}

function esc(s) {
  return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function ago(ms) {
  const s = Math.max(0, (Date.now() - ms) / 1000);
  if (s < 90) return "just now";
  if (s < 3600) return Math.round(s / 60) + " min ago";
  if (s < 86400 * 2) return Math.round(s / 3600) + " h ago";
  return Math.round(s / 86400) + " days ago";
}

function randomHex(bytes) {
  const a = new Uint8Array(bytes);
  crypto.getRandomValues(a);
  return [...a].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function json(obj, status = 200) {
  return new Response(JSON.stringify(obj), { status, headers: { "Content-Type": "application/json; charset=utf-8" } });
}

function text(s, status = 200) {
  return new Response(s, { status, headers: { "Content-Type": "text/plain; charset=utf-8" } });
}

const LANGS = {
  c: "c", h: "cpp", cpp: "cpp", cc: "cpp", hpp: "cpp", cs: "csharp", py: "python", js: "javascript", mjs: "javascript",
  ts: "typescript", tsx: "tsx", jsx: "jsx", java: "java", kt: "kotlin", swift: "swift", go: "go", rs: "rust", rb: "ruby",
  php: "php", html: "html", htm: "html", xml: "xml", css: "css", scss: "scss", json: "json", yml: "yaml", yaml: "yaml",
  sql: "sql", sh: "bash", bat: "batch", cmd: "batch", ps1: "powershell", lua: "lua", ini: "ini", md: "markdown",
  vb: "vbnet", pl: "perl", r: "r", dart: "dart", toml: "toml",
};
