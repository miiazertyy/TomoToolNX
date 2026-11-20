// uisound_stems.js — runs inside headless Chrome (see render_sounds.mjs).
//
// The voices below are LLM-Selfbot's (webui/src/lib/uisound.ts), with the
// TypeScript removed and nothing else changed: the same building blocks, the
// same numbers. What differs is where they go. LLM-Selfbot sends every voice
// to the master out (dry) and part of it into a reverb room (wet). Here the
// two are kept apart, so the Switch can play the dry part and feed the wet
// part through its own room and compressor (source/ui_sound.cpp):
//   channel 0 = dry (what reaches `out` directly)
//   channel 1 = wet send (what reaches `room`, before the room)
// Randomness (the noise, each click's rate, band and offset) comes from a
// seeded generator so every take is reproducible.
(function () {
  "use strict";

  let rnd = Math.random;
  function seeded(seed) {
    let a = seed >>> 0;
    return function () {            // mulberry32
      a = (a + 0x6d2b79f5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }

  let g = null;        // { a, dry, wet, noise }
  let level = 1;
  const lead = 0;

  function graph(a) {
    const merge = a.createChannelMerger(2);
    merge.connect(a.destination);
    const dry = a.createGain();
    dry.gain.value = 1;
    dry.connect(merge, 0, 0);
    const wet = a.createGain();
    wet.gain.value = 1;
    wet.connect(merge, 0, 1);
    const noise = a.createBuffer(1, Math.floor(a.sampleRate * 0.4), a.sampleRate);
    const d = noise.getChannelData(0);
    for (let i = 0; i < d.length; i++) d[i] = rnd() * 2 - 1;
    return { a, dry, wet, noise };
  }

  function send(node, wet) {
    node.connect(g.dry);
    if (wet > 0) {
      const w = g.a.createGain();
      w.gain.value = wet;
      node.connect(w).connect(g.wet);
    }
  }

  function click(at, freq, dur, peak, q = 2, wet = 0.04) {
    const a = g.a;
    const t = a.currentTime + lead + at;
    const src = a.createBufferSource();
    src.buffer = g.noise;
    src.playbackRate.value = 0.9 + rnd() * 0.2;
    const band = a.createBiquadFilter();
    band.type = "bandpass";
    band.frequency.value = freq * (0.97 + rnd() * 0.06);
    band.Q.value = q;
    const env = a.createGain();
    env.gain.setValueAtTime(0.0001, t);
    env.gain.exponentialRampToValueAtTime(peak * level, t + 0.0012);
    env.gain.exponentialRampToValueAtTime(0.0001, t + dur);
    src.connect(band).connect(env);
    send(env, wet);
    src.start(t, rnd() * 0.3);
    src.stop(t + dur + 0.02);
  }

  function tone(at, freq, dur, peak, { type = "sine", wet = 0.2, glide = 0, attack = 0.006, detune = 0 } = {}) {
    const a = g.a;
    const t = a.currentTime + lead + at;
    const o = a.createOscillator();
    o.type = type;
    o.frequency.setValueAtTime(freq, t);
    if (glide) o.frequency.exponentialRampToValueAtTime(glide, t + dur * 0.7);
    o.detune.value = detune;
    const env = a.createGain();
    env.gain.setValueAtTime(0.0001, t);
    env.gain.exponentialRampToValueAtTime(peak * level, t + attack);
    env.gain.exponentialRampToValueAtTime(0.0001, t + dur);
    o.connect(env);
    send(env, wet);
    o.start(t);
    o.stop(t + dur + 0.05);
  }

  function bell(at, freq, dur, peak, wet = 0.38) {
    tone(at, freq, dur, peak, { wet });
    tone(at, freq, dur * 0.9, peak * 0.45, { wet, detune: 7 });
    tone(at, freq * 2.76, dur * 0.3, peak * 0.12, { wet });
  }

  function swish(at, from, to, dur, peak) {
    const a = g.a;
    const t = a.currentTime + lead + at;
    const src = a.createBufferSource();
    src.buffer = g.noise;
    const band = a.createBiquadFilter();
    band.type = "bandpass";
    band.Q.value = 1.4;
    band.frequency.setValueAtTime(from, t);
    band.frequency.exponentialRampToValueAtTime(to, t + dur);
    const env = a.createGain();
    env.gain.setValueAtTime(0.0001, t);
    env.gain.exponentialRampToValueAtTime(peak * level, t + dur * 0.35);
    env.gain.exponentialRampToValueAtTime(0.0001, t + dur);
    src.connect(band).connect(env);
    send(env, 0.25);
    src.start(t);
    src.stop(t + dur + 0.02);
  }

  const PENTA = [0, 2, 4, 7, 9, 12, 14, 16, 19, 21, 24];

  function voices(name, { p = 0.5, strong = false } = {}) {
    switch (name) {
      case "tap":
        click(0, 2200, 0.03, 1.3, 1.2, 0.05);
        tone(0, 300, 0.05, 0.12, { wet: 0, attack: 0.001 });
        break;
      case "up":
        click(0, 3100, 0.02, 0.8, 1.5, 0.05);
        tone(0, 470, 0.035, 0.06, { wet: 0, attack: 0.001 });
        break;
      case "tick":
        click(0, 3200 + 1400 * p, strong ? 0.024 : 0.018, strong ? 1.4 : 0.95, 3, 0.06);
        tone(0, 2200 + 600 * p, 0.015, strong ? 0.08 : 0.05, { wet: 0, attack: 0.0008 });
        break;
      case "on":
        click(0, 2700, 0.024, 0.7, 1.8, 0.05);
        click(0.036, 4000, 0.02, 0.55, 2.2, 0.07);
        tone(0.03, 1046.5, 0.18, 0.06, { wet: 0.35 });
        break;
      case "off":
        click(0, 3800, 0.022, 0.7, 2.2, 0.05);
        click(0.036, 2400, 0.024, 0.65, 1.8, 0.07);
        tone(0.03, 698.5, 0.16, 0.055, { wet: 0.35 });
        break;
      case "select":
        click(0, 3000, 0.022, 2.0, 2.4, 0.06);
        bell(0.008, 1318.5, 0.4, 0.07, 0.35);
        break;
      case "open":
        tone(0, 620, 0.12, 0.14, { glide: 930, wet: 0.25 });
        click(0, 2200, 0.016, 1.0, 1.6, 0.03);
        break;
      case "menu":
        click(0, 3400, 0.014, 0.9, 2.6, 0.05);
        tone(0, 540, 0.09, 0.12, { glide: 820, wet: 0.28, attack: 0.004 });
        tone(0.018, 1620, 0.24, 0.025, { wet: 0.45 });
        break;
      case "ok":
        bell(0, 880, 1.1, 0.12);
        bell(0.095, 1318.5, 1.4, 0.1);
        break;
      case "info":
        bell(0, 1046.5, 1.0, 0.17);
        tone(0.06, 1568, 0.6, 0.03, { wet: 0.45 });
        break;
      case "err":
        tone(0, 659.3, 0.24, 0.12, { type: "triangle", glide: 622, wet: 0.18 });
        tone(0.13, 523.3, 0.38, 0.12, { type: "triangle", glide: 494, wet: 0.22 });
        break;
      case "send":
        swish(0, 500, 4200, 0.2, 0.55);
        tone(0.02, 700, 0.18, 0.06, { glide: 1400, wet: 0.25 });
        break;
      case "receive":
        bell(0, 1567.98, 0.7, 0.15, 0.32);
        tone(0, 784, 0.12, 0.08, { wet: 0.1 });
        break;
      case "delete":
        tone(0, 520, 0.16, 0.14, { glide: 250, wet: 0.12 });
        click(0, 1700, 0.022, 1.2, 1.4, 0.03);
        break;
      case "undo":
        tone(0, 300, 0.16, 0.08, { glide: 640, wet: 0.18 });
        click(0.1, 3000, 0.016, 0.35, 2.2, 0.05);
        break;
      case "wind":
        click(0, 2600 + 1600 * p, strong ? 0.02 : 0.014, strong ? 0.8 : 0.55, 3.2, 0.03);
        tone(0, 1900 + 700 * p, 0.012, strong ? 0.04 : 0.025, { wet: 0, attack: 0.0008 });
        break;
      case "slide": {
        const v = 0.97 + rnd() * 0.06;
        const note = Math.pow(2, PENTA[Math.round(Math.min(1, Math.max(0, p)) * 7)] / 12);
        swish(0, 700, 2400, 0.17, 0.07);
        click(0.19, 2700 * v, 0.024, 0.42, 1.8, 0.05);
        click(0.226, 4000 * v, 0.02, 0.33, 2.2, 0.07);
        tone(0.22, 1046.5 * note * (0.995 + rnd() * 0.01), 0.18, 0.036, { wet: 0.35 });
        break;
      }
      case "rise": {
        const f = 440 * Math.pow(2, (Math.round(p * 7) * 2) / 12);
        tone(0, f, 0.24, 0.09, { glide: f * 1.5, wet: 0.3, attack: 0.02 });
        click(0, 2400, 0.012, 0.35, 2, 0.04);
        break;
      }
      case "done":
        bell(0, 1318.5, 0.9, 0.06);
        bell(0.075, 1760, 1.1, 0.05);
        break;
      case "key":
        if (strong) {
          click(0, 1700 + rnd() * 300, 0.02, 0.3, 1.6, 0.03);
          tone(0, 210, 0.03, 0.03, { wet: 0, attack: 0.001 });
        } else {
          click(0, 3400 + rnd() * 600, 0.012, 0.25, 3, 0.02);
        }
        break;
      case "land":
        click(0, 1500, 0.024, 0.5, 1.2, 0.05);
        tone(0, 330, 0.09, 0.05, { glide: 520, wet: 0.15, attack: 0.003 });
        tone(0.05, 1568, 0.25, 0.012, { wet: 0.5 });
        break;
      case "pluck": {
        const f = 523.25 * Math.pow(2, PENTA[Math.round(Math.min(1, Math.max(0, p)) * 10)] / 12);
        tone(0, f, 0.16, 0.066, { wet: 0.22, attack: 0.002 });
        tone(0, f * 3.93, 0.045, 0.013, { wet: 0.1, attack: 0.001 });
        click(0, 2600, 0.008, 0.2, 2, 0.03);
        break;
      }
      case "zip":
        swish(0, 900, 3600, 0.2, 0.11);
        break;
      case "seat":
        if (strong) {
          click(0, 900, 0.026, 0.5, 1.2, 0.04);
          tone(0, 150, 0.07, 0.05, { wet: 0.05, attack: 0.002 });
        } else {
          click(0, 1300 + 700 * p, 0.02, 0.34, 1.6, 0.04);
          tone(0, 240 + 160 * p, 0.06, 0.035, { wet: 0.08, attack: 0.002 });
        }
        break;
      case "glint": {
        const f = 1046.5 * Math.pow(2, PENTA[Math.round(Math.min(1, Math.max(0, p)) * 10)] / 12);
        tone(0, f, 0.11, 0.042, { wet: 0.4, attack: 0.001 });
        tone(0, f * 3.01, 0.04, 0.01, { wet: 0.3, attack: 0.001 });
        if (strong) tone(0.06, f * 1.5, 0.3, 0.03, { wet: 0.5, attack: 0.002 });
        break;
      }
      case "dawn":
        tone(0, 196, 0.9, 0.09, { glide: 233, wet: 0.5, attack: 0.18 });
        tone(0.05, 293.7, 0.85, 0.07, { glide: 349, wet: 0.5, attack: 0.2 });
        swish(0.04, 300, 1500, 0.5, 0.1);
        bell(0.34, 1318.5, 1.2, 0.05, 0.55);
        break;
      case "whoosh":
        swish(0, 420, 1900, 0.26, 0.22);
        click(0.2, 1500, 0.016, 0.22, 1.4, 0.05);
        break;
      case "settle":
        tone(0, 160, 0.11, 0.06, { glide: 115, wet: 0.12, attack: 0.004 });
        click(0, 950, 0.018, 0.22, 1.1, 0.04);
        break;
      case "grab":
        click(0, 1800, 0.018, 0.9, 1.6, 0.03);
        tone(0, 240, 0.045, 0.07, { wet: 0, attack: 0.001 });
        break;
      case "release":
        click(0, 3000, 0.014, 0.6, 2.2, 0.04);
        tone(0, 420, 0.035, 0.035, { wet: 0.1, attack: 0.001 });
        break;
      default:
        throw new Error("unknown voice " + name);
    }
  }

  // Render one take. Returns little-endian int16 PCM for both stems, base64,
  // trimmed where both have died away (-80 dBFS), with a 4 ms fade at the end.
  async function renderStems(name, p, strong, seed, rate, seconds) {
    rnd = seeded(seed);
    const off = new OfflineAudioContext(2, Math.ceil(rate * seconds), rate);
    g = graph(off);
    level = 1;
    try {
      voices(name, { p, strong });
    } finally {
      rnd = Math.random;
    }
    const buf = await off.startRendering();
    g = null;
    const d = buf.getChannelData(0), w = buf.getChannelData(1);
    const floor = 0.0001;   // -80 dBFS
    let last = 0, peakD = 0, peakW = 0;
    for (let i = 0; i < d.length; i++) {
      const a = Math.abs(d[i]), b = Math.abs(w[i]);
      if (a > floor || b > floor) last = i;
      if (a > peakD) peakD = a;
      if (b > peakW) peakW = b;
    }
    const n = Math.min(d.length, last + 1 + Math.round(rate * 0.004));
    const fade = Math.min(n, Math.round(rate * 0.004));
    function pack(src) {
      const out = new Int16Array(n);
      for (let i = 0; i < n; i++) {
        let v = src[i];
        if (i >= n - fade) v *= (n - i) / fade;
        v = Math.max(-1, Math.min(1, v));
        out[i] = Math.round(v * 32767);
      }
      const bytes = new Uint8Array(out.buffer);
      let s = "";
      for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
      return btoa(s);
    }
    return { n, peakD, peakW, dry: pack(d), wet: pack(w) };
  }

  window.renderStems = renderStems;
})();
