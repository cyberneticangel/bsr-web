// Integration test of the browser layer in Node: real bsr.js/bsr.wasm/game.js with stubbed DOM, WebGL2 and
// Web Audio. Exercises menu building, data fetch into MEMFS, bsr_start, the main loop, HUD/events and audio decoding.
// usage: node tools/nodetest.js [track] [class] [seconds]
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const WEB = path.join(__dirname, '..', 'web');
const track = process.argv[2] || 'baanbreker';
const cls = process.argv[3] || 'pro_std';
const seconds = +(process.argv[4] || 8);

// ---- fake DOM
const elements = {};
function el(id) {
  if (!elements[id]) {
    const e = {
      id, children: [], style: {}, dataset: {}, value: '', checked: true, disabled: false, textContent: '', innerHTML: '',
      title: '', width: 1280, height: 720, clientWidth: 1280, clientHeight: 720,
      classList: { _s: new Set(), add(c) { this._s.add(c); }, remove(c) { this._s.delete(c); },
        toggle(c, on) { (on === undefined ? !this._s.has(c) : on) ? this._s.add(c) : this._s.delete(c); }, contains(c) { return this._s.has(c); } },
      appendChild(c) { this.children.push(c); return c; },
      addEventListener() {}, removeEventListener() {}, focus() {},
      getBoundingClientRect() { return { left: 0, top: 0, right: 1280, bottom: 720, width: 1280, height: 720 }; },
      getContext: (type) => (type === 'webgl2' ? fakeGL() : null),
      toDataURL: () => 'data:image/png;base64,',
    };
    Object.defineProperty(e, 'innerHTML', { get() { return this._html || ''; }, set(v) { this._html = v; if (v === '') this.children = []; } });
    elements[id] = e;
  }
  return elements[id];
}
const document = {
  getElementById: el, querySelector: (s) => el(s.replace('#', '')), querySelectorAll: () => [],
  createElement: (t) => { const e = el('__' + t + Math.random()); e.tagName = t; return e; },
  addEventListener() {}, hidden: false, body: el('body'),
};

// ---- fake WebGL2: every call succeeds; counts draws
let glCalls = 0, drawCalls = 0, texUploads = 0;
function fakeGL() {
  const consts = new Proxy({}, { get: (_, k) => (typeof k === 'string' && /^[A-Z_0-9]+$/.test(k) ? 0x1000 + k.length : undefined) });
  let id = 1;
  const gl = new Proxy({}, {
    get(target, k) {
      if (k in target) return target[k];  // properties Emscripten stores on the context (currentProgram, ...)
      if (k === 'canvas') return el('canvas');
      if (k === 'drawingBufferWidth') return 1280;
      if (k === 'drawingBufferHeight') return 720;
      if (typeof k === 'string' && /^[A-Z_0-9]+$/.test(k)) return consts[k];
      return (...a) => {
        glCalls++;
        if (k === 'drawElements' || k === 'drawArrays') drawCalls++;
        if (k === 'texImage2D') texUploads++;
        if (/^create/.test(k)) return { id: id++ };
        if (k === 'shaderSource') { a[0].src = a[1]; return undefined; }
        if (k === 'attachShader') { (a[0].shaders = a[0].shaders || []).push(a[1]); return undefined; }
        const uniforms = (prog) => (prog.shaders || []).flatMap((sh) => [...(sh.src || '').matchAll(/uniform\s+\w+\s+([^;]+);/g)]
          .flatMap((m) => m[1].split(',').map((n) => n.trim())));
        if (k === 'getProgramParameter' && a[1] === 35718 /* ACTIVE_UNIFORMS */) return uniforms(a[0]).length;
        if (k === 'getShaderParameter' || k === 'getProgramParameter') return true;
        if (k === 'getActiveUniform') return { name: uniforms(a[0])[a[1]], size: 1, type: 0 };
        if (k === 'getParameter') return a[0] === 0x1F02 ? 'WebGL 2.0' : 8;
        if (k === 'getSupportedExtensions') return ['EXT_texture_filter_anisotropic'];
        if (k === 'getExtension') return {};
        if (k === 'getContextAttributes') return { antialias: true };
        if (k === 'getUniformLocation') return { id: id++ };
        if (k === 'getError') return 0;
        if (k === 'getShaderInfoLog' || k === 'getProgramInfoLog') return '';
        if (k === 'isContextLost') return false;
        return undefined;
      };
    },
  });
  return gl;
}

// ---- fake Web Audio
const audioLog = { decoded: 0, played: 0 };
class FakeAudioContext {
  constructor() { this.currentTime = 0; this.state = 'running'; this.destination = {}; }
  createGain() { return { gain: { value: 1, setTargetAtTime() {} }, connect: (x) => x }; }
  createBuffer(ch, n, rate) {
    audioLog.decoded++;
    const data = Array.from({ length: ch }, () => new Float32Array(n));
    return { numberOfChannels: ch, length: n, sampleRate: rate, getChannelData: (c) => data[c], _data: data };
  }
  createBufferSource() {
    return { playbackRate: { value: 1, setTargetAtTime() {} }, connect: (x) => x, start() { audioLog.played++; }, stop() {}, loop: false };
  }
  resume() {}
}

// ---- fetch from web/
async function fakeFetch(url, opts) {
  if (opts && opts.method === 'POST') return { ok: true };
  const file = path.join(WEB, decodeURIComponent(String(url)).replace(/^\//, ''));
  if (!fs.existsSync(file)) return { ok: false, status: 404 };
  const buf = fs.readFileSync(file);
  return { ok: true, arrayBuffer: async () => buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length), json: async () => JSON.parse(buf) };
}

const hudLog = [], events = [];
let raf = [];
const ctx = {
  console, document, window: null, navigator: { userAgent: 'node', getGamepads: () => [] },
  location: { search: '', href: 'http://localhost/' }, URLSearchParams, URL: { createObjectURL: () => 'blob:x', revokeObjectURL() {} },
  Blob: class { constructor(parts) { this.size = parts.reduce((s, p) => s + p.length, 0); } },
  fetch: fakeFetch, performance: { now: () => Date.now() }, setTimeout, clearTimeout, setInterval, clearInterval,
  requestAnimationFrame: (f) => raf.push(f), AudioContext: FakeAudioContext, WebAssembly, TextDecoder, TextEncoder,
  Audio: class { constructor() { this.loop = false; this.volume = 1; } play() { audioLog.music = true; return Promise.resolve(); } pause() {} },
  WebGLRenderingContext: class {}, WebGL2RenderingContext: class {},
  addEventListener() {}, removeEventListener() {}, devicePixelRatio: 1, innerWidth: 1280, innerHeight: 720,
  getComputedStyle: () => ({ width: '1280px', height: '720px' }),
};
ctx.window = ctx;
ctx.self = ctx;
ctx.globalThis = ctx;
vm.createContext(ctx);

// Emscripten's file locator uses scriptDirectory; read the wasm ourselves.
const gameJs = fs.readFileSync(path.join(WEB, 'game.js'), 'utf8')
  .replace("var Module = {", "var Module = { wasmBinary: __wasm, locateFile: (p) => p,");
ctx.__wasm = fs.readFileSync(path.join(WEB, 'bsr.wasm'));
vm.runInContext(gameJs + '\n;globalThis.Module = Module; globalThis.__audio = Audio; globalThis.__startRace = startRace;', ctx, { filename: 'game.js' });
const M = ctx.Module;
const origHud = M.onHud, origEvent = M.onEvent;
M.onHud = (j) => { hudLog.push(JSON.parse(j)); origHud.call(M, j); };
M.onEvent = (n) => { events.push(n); origEvent.call(M, n); };
vm.runInContext(fs.readFileSync(path.join(WEB, 'bsr.js'), 'utf8'), ctx, { filename: 'bsr.js' });

function fail(msg) { console.error('FAIL:', msg); process.exit(1); }

(async () => {
  for (let i = 0; i < 200 && !ctx.document.getElementById('cls').children.length; i++) await new Promise((r) => setTimeout(r, 50));
  if (!el('cls').children.length) fail('menu not built (manifest/bsr_init) status=' + el('status').innerHTML);
  console.log('menu ok: tracks=%d classes=%d weather=%d', el('tracks').children.length, el('cls').children.length, el('weather').children.length);
  await ctx.__startRace({ track, cls, skin: 1, opponents: 5, laps: 1, weather: 'sunset_orange' });
  if (/Error/.test(el('status').innerHTML)) fail('startRace: ' + el('status').innerHTML);
  if (!el('menu').classList.contains('hidden')) fail('menu still visible after start');
  // Drive the main loop with simulated time (Emscripten uses requestAnimationFrame + performance.now).
  let now = Date.now();
  ctx.performance.now = () => now;
  M.ccall('bsr_autopilot', null, ['number'], [1]);
  const frames = Math.round(seconds * 60);
  for (let f = 0; f < frames; f++) {
    now += 1000 / 60;
    const q = raf; raf = [];
    q.forEach((fn) => fn(now));
  }
  const last = hudLog[hudLog.length - 1] || {};
  console.log('frames=%d glCalls=%d draws=%d textures=%d hud=%d events=%s', frames, glCalls, drawCalls, texUploads, hudLog.length, events.join(','));
  console.log('last hud:', JSON.stringify(last));
  console.log('audio buffers decoded=%d played=%d music=%s', audioLog.decoded, audioLog.played, !!audioLog.music);
  const eng = Object.entries(ctx.__audio.buffers).filter(([, b]) => b).map(([k, b]) => {
    const d = b.getChannelData(0); let e = 0, df = 0;
    for (let i = 1; i < d.length; i++) { e += d[i] * d[i]; df += (d[i] - d[i - 1]) ** 2; }
    return `${k}:${b.sampleRate}Hz smooth=${(df / Math.max(e, 1e-9)).toFixed(2)}`;
  });
  console.log('decoded sounds (smooth<1 means decryption ok):', eng.join(' '));
  if (drawCalls < 100) fail('too few draw calls');
  if (!events.includes('go')) fail('race never started');
  if (!(last.speed > 5)) fail('player car not moving');
  if (seconds > 40 && !events.includes('finish')) fail('race not finished');
  console.log('PASS');
  process.exit(0);
})().catch((e) => fail(e.stack || e));
