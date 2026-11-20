// render_sounds.mjs — build romfs/sfx/ui.bank from LLM-Selfbot's own voices.
//
//   node tools/sounds/render_sounds.mjs            (writes romfs/sfx/ui.bank)
//
// Needs Google Chrome (or Chromium/Edge: set CHROME=path). Each voice variant
// is rendered by uisound_stems.js in a headless browser with WebAudio, the
// engine LLM-Selfbot itself plays them with, into a dry stem and a reverb-send
// stem. The Switch mixes them through its own room and compressor
// (source/ui_sound.cpp).
//
// Bank layout (little endian), then zlib-compressed behind an 8-byte header:
//   "TTSZ" u32 rawSize | zlib( raw )
//   raw:  "TTSB" u32 version(1) u32 rate u32 count
//         count x { char name[12]; f32 p; u8 strong; u8 take; u16 0; u32 frames; u32 dry; u32 wet }
//         int16 samples (dry/wet are sample offsets into this area; wet = 0xFFFFFFFF if silent)
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { tmpdir } from "node:os";
import { fileURLToPath } from "node:url";
import zlib from "node:zlib";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "..", "..");
const RATE = 48000;

const CHROME = process.env.CHROME || [
  "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
  "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
  "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
  "/usr/bin/google-chrome", "/usr/bin/chromium", "/usr/bin/chromium-browser",
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
].find((p) => existsSync(p));
if (!CHROME) { console.error("Chrome not found; set CHROME=/path/to/chrome"); process.exit(1); }

// ── What to render ──────────────────────────────────────────────────────────
const LONG = { ok: 1.7, info: 1.25, done: 1.3, dawn: 1.75, receive: 0.85, err: 0.65 };
const variants = [];
const add = (name, p, strong, take) => variants.push({ name, p, strong, take });
const steps = (n) => Array.from({ length: n }, (_, i) => (n === 1 ? 0.5 : i / (n - 1)));
for (const p of steps(9)) for (const s of [0, 1]) for (const t of [0, 1]) add("tick", p, s, t);
for (const p of steps(9)) for (const s of [0, 1]) add("wind", p, s, 0);
for (const p of steps(9)) add("seat", p, 0, 0);
add("seat", 0.5, 1, 0);
for (const p of steps(8)) add("slide", p, 0, 0);
for (const p of steps(8)) add("rise", p, 0, 0);
for (const p of steps(11)) add("pluck", p, 0, 0);
for (const p of steps(11)) add("glint", p, 0, 0);
add("glint", 0.8, 1, 0);
for (const n of ["tap", "up"]) for (const t of [0, 1, 2]) add(n, 0.5, 0, t);
for (const n of ["on", "off", "select", "open", "menu", "delete", "undo", "zip", "settle", "whoosh",
                 "grab", "release", "land", "send"]) for (const t of [0, 1]) add(n, 0.5, 0, t);
for (const s of [0, 1]) for (const t of [0, 1]) add("key", 0.5, s, t);
for (const n of ["ok", "info", "err", "done", "dawn", "receive"]) add(n, 0.5, 0, 0);

function seedOf(v) {
  let h = 2166136261;
  for (const c of `${v.name}|${v.p.toFixed(4)}|${v.strong}|${v.take}`) h = Math.imul(h ^ c.charCodeAt(0), 16777619) >>> 0;
  return h;
}

// ── Chrome over the DevTools protocol ───────────────────────────────────────
const port = 9341;
const profile = join(tmpdir(), "ttnx-sound-render");
const chrome = spawn(CHROME, ["--headless=new", "--disable-gpu", "--mute-audio", `--remote-debugging-port=${port}`,
  `--user-data-dir=${profile}`, "--no-first-run", "--no-default-browser-check", "about:blank"], { stdio: "ignore" });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function json(path) {
  for (let i = 0; i < 80; i++) {
    try { return await (await fetch(`http://127.0.0.1:${port}${path}`)).json(); } catch { await sleep(150); }
  }
  throw new Error("Chrome did not start");
}
const page = (await json("/json/list")).find((p) => p.type === "page");
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener("open", r, { once: true }));
let nextId = 1;
const waiting = new Map();
ws.addEventListener("message", (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
});
const send = (method, params = {}) => new Promise((r) => { const id = nextId++; waiting.set(id, r); ws.send(JSON.stringify({ id, method, params })); });
async function evaluate(expression) {
  const r = await send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true });
  if (r.result?.exceptionDetails) throw new Error(JSON.stringify(r.result.exceptionDetails));
  return r.result?.result?.value;
}

try {
  await evaluate(readFileSync(join(here, "uisound_stems.js"), "utf8"));
  const chunks = [];
  const entries = [];
  let cursor = 0;
  for (const v of variants) {
    const secs = LONG[v.name] ?? 0.65;
    const r = await evaluate(`renderStems(${JSON.stringify(v.name)}, ${v.p}, ${!!v.strong}, ${seedOf(v)}, ${RATE}, ${secs})`);
    const dry = Buffer.from(r.dry, "base64");
    const wet = Buffer.from(r.wet, "base64");
    const e = { ...v, frames: r.n, dry: cursor, wet: 0xffffffff };
    chunks.push(dry); cursor += r.n;
    if (r.peakW > 0.00003) { e.wet = cursor; chunks.push(wet); cursor += r.n; }
    entries.push(e);
    process.stdout.write(`\r${entries.length}/${variants.length} ${v.name.padEnd(8)} ${(r.n / RATE * 1000).toFixed(0).padStart(5)} ms `
      + `peak ${(20 * Math.log10(r.peakD || 1e-9)).toFixed(1)} dB   `);
  }
  process.stdout.write("\n");

  const head = Buffer.alloc(16 + entries.length * 36);
  head.write("TTSB", 0, "latin1");
  head.writeUInt32LE(1, 4);
  head.writeUInt32LE(RATE, 8);
  head.writeUInt32LE(entries.length, 12);
  entries.forEach((e, i) => {
    const o = 16 + i * 36;
    head.write(e.name.slice(0, 11), o, "latin1");
    head.writeFloatLE(e.p, o + 12);
    head.writeUInt8(e.strong ? 1 : 0, o + 16);
    head.writeUInt8(e.take, o + 17);
    head.writeUInt32LE(e.frames, o + 20);
    head.writeUInt32LE(e.dry, o + 24);
    head.writeUInt32LE(e.wet, o + 28);
  });
  const raw = Buffer.concat([head, ...chunks]);
  const packed = zlib.deflateSync(raw, { level: 9 });
  const out = Buffer.alloc(8);
  out.write("TTSZ", 0, "latin1");
  out.writeUInt32LE(raw.length, 4);
  const dest = join(root, "romfs", "sfx", "ui.bank");
  mkdirSync(dirname(dest), { recursive: true });
  writeFileSync(dest, Buffer.concat([out, packed]));
  console.log(`${entries.length} variants, ${(raw.length / 1048576).toFixed(2)} MB raw, `
    + `${(packed.length / 1048576).toFixed(2)} MB packed -> ${dest}`);
} finally {
  ws.close();
  chrome.kill();
}
process.exit(0);
