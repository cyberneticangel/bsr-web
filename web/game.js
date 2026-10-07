// Front-end for the Big Scale Racing WebAssembly build: menu, data loading, HUD, audio, touch input.
'use strict';

const KEY = new Uint8Array([0x47, 0xc3, 0xf5, 0x12, 0x38, 0xe9, 0xb5, 0x91, 0x25, 0x63, 0x06, 0xd9, 0xaa, 0x6f, 0x3a, 0x73]);
const CLASS_NAMES = {
  junior_std: 'Junior', junior_hop: 'Junior Hop-up', trained_std: 'Trained', trained_hop: 'Trained Hop-up',
  skilled_std: 'Skilled', skilled_hop: 'Skilled Hop-up', expert_std: 'Expert', expert_hop: 'Expert Hop-up',
  pro_std: 'Pro', pro_hop: 'Pro Hop-up', Monster: 'Monster Truck',
};
const params = new URLSearchParams(location.search);
const $ = (id) => document.getElementById(id);

let manifest = null;
const loaded = new Set();
const sel = { track: 'baanbreker', cls: 'pro_std', skin: 0 };
let lastRace = null;
let hudState = {};

function xorInPlace(u8, start = 0) {
  for (let i = start; i < u8.length; i++) u8[i] ^= KEY[(i - start) & 15];
  return u8;
}

// ------------------------------------------------------------------ audio
const Audio = {
  ctx: null, buffers: {}, engine: null, skid: null, crowd: null, master: null, music: null, musicEl: null,
  lastImpact: 0, enabled: true,
  init() {
    if (this.ctx) return;
    try {
      this.ctx = new (window.AudioContext || window.webkitAudioContext)();
      this.master = this.ctx.createGain();
      this.master.gain.value = 0.8;
      this.master.connect(this.ctx.destination);
    } catch (e) { this.ctx = null; }
  },
  // .fsw = RIFF WAVE with plain header; sample data XOR-encrypted from the start of the data chunk.
  decodeFSW(u8) {
    const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    let o = 12, fmt = null;
    while (o + 8 <= u8.length) {
      const id = String.fromCharCode(u8[o], u8[o + 1], u8[o + 2], u8[o + 3]);
      const size = dv.getUint32(o + 4, true);
      if (id === 'fmt ') fmt = { ch: dv.getUint16(o + 10, true), rate: dv.getUint32(o + 12, true), bits: dv.getUint16(o + 22, true) };
      if (id === 'data' && fmt) {
        const data = xorInPlace(u8.slice(o + 8, o + 8 + size));
        const n = Math.floor(data.length / (fmt.bits / 8) / fmt.ch);
        const buf = this.ctx.createBuffer(fmt.ch, Math.max(1, n), fmt.rate);
        const ddv = new DataView(data.buffer);
        for (let c = 0; c < fmt.ch; c++) {
          const out = buf.getChannelData(c);
          for (let i = 0; i < n; i++) {
            out[i] = fmt.bits === 16 ? ddv.getInt16((i * fmt.ch + c) * 2, true) / 32768 : (data[i * fmt.ch + c] - 128) / 128;
          }
        }
        return buf;
      }
      o += 8 + size + (size & 1);
    }
    return null;
  },
  load(name) {
    if (!this.ctx || this.buffers[name] !== undefined) return;
    const path = manifest.sounds[name];
    if (!path) return;
    try { this.buffers[name] = this.decodeFSW(Module.FS.readFile('/data/' + path)); } catch (e) { this.buffers[name] = null; }
  },
  play(name, vol = 1, rate = 1) {
    if (!this.ctx || !this.enabled) return null;
    this.load(name);
    const b = this.buffers[name];
    if (!b) return null;
    const src = this.ctx.createBufferSource();
    src.buffer = b;
    src.playbackRate.value = rate;
    const g = this.ctx.createGain();
    g.gain.value = vol;
    src.connect(g).connect(this.master);
    src.start();
    return { src, g };
  },
  loop(name, vol) {
    const v = this.play(name, vol);
    if (v) v.src.loop = true;
    return v;
  },
  startRace(cls) {
    this.stopRace();
    if (!this.ctx) return;
    this.engine = this.loop(cls.endsWith('hop') ? 'FX_Engine_st_22_ZENOAH_A' : 'FX_Engine_st_22_serpent_A', 0);
    this.skid = this.loop('FX_tire_slip_A', 0);
    this.crowd = this.loop('FX_env_crowd_gen_A_22', 0.12);
  },
  stopRace() {
    for (const k of ['engine', 'skid', 'crowd']) {
      if (this[k]) { try { this[k].src.stop(); } catch (e) {} this[k] = null; }
    }
  },
  update(rpm, throttle, skid, impact, speed) {
    if (!this.ctx) return;
    const t = this.ctx.currentTime;
    if (this.engine) {
      this.engine.src.playbackRate.setTargetAtTime(0.55 + rpm * 1.5, t, 0.05);
      this.engine.g.gain.setTargetAtTime(0.18 + throttle * 0.22, t, 0.05);
    }
    if (this.skid) this.skid.g.gain.setTargetAtTime(Math.min(0.5, skid * 0.6) * (speed > 1.5 ? 1 : 0), t, 0.05);
    if (impact > 1.2 && performance.now() - this.lastImpact > 180) {
      this.lastImpact = performance.now();
      this.play(impact > 3 ? 'FX_auto_botst_c' : 'AU_FX_botst_alg_A', Math.min(1, impact / 5), 0.9 + Math.random() * 0.2);
    }
  },
  playMusic(path) {
    this.stopMusic();
    if (!path || !$('music').checked) return;
    try {
      const u8 = xorInPlace(Module.FS.readFile('/data/' + path).slice());  // .fs3 = MP3 XORed with the key
      const url = URL.createObjectURL(new Blob([u8], { type: 'audio/mpeg' }));
      const el = new window.Audio(url);
      el.loop = true;
      el.volume = 0.45;
      el.play().catch(() => {});
      this.musicEl = el;
    } catch (e) {}
  },
  stopMusic() {
    if (this.musicEl) { this.musicEl.pause(); URL.revokeObjectURL(this.musicEl.src); this.musicEl = null; }
  },
};

// ------------------------------------------------------------------ data loading
async function fetchFiles(list, onProgress) {
  const todo = list.filter((f) => !loaded.has(f));
  let done = 0;
  const workers = 6;
  let next = 0;
  async function worker() {
    while (next < todo.length) {
      const f = todo[next++];
      const r = await fetch('data/' + f);
      if (!r.ok) throw new Error('missing data/' + f);
      const buf = new Uint8Array(await r.arrayBuffer());
      const dir = '/data/' + f.substring(0, f.lastIndexOf('/'));
      Module.FS.mkdirTree(dir);
      Module.FS.writeFile('/data/' + f, buf);
      loaded.add(f);
      onProgress(++done, todo.length);
    }
  }
  await Promise.all(Array.from({ length: workers }, worker));
}

function setStatus(text, frac) {
  $('status').innerHTML = text + (frac !== undefined ? `<div class="bar"><div style="width:${(frac * 100).toFixed(1)}%"></div></div>` : '');
}

// ------------------------------------------------------------------ menu
function buildMenu() {
  const tracks = $('tracks');
  tracks.innerHTML = '';
  for (const t of Object.keys(manifest.tracks)) {
    const b = document.createElement('button');
    b.textContent = manifest.trackNames[t] || t;
    b.onclick = () => { sel.track = t; refreshSel(); Audio.init(); Audio.play('menu_updown', 0.5); };
    b.dataset.v = t;
    tracks.appendChild(b);
  }
  const cls = $('cls');
  cls.innerHTML = '';
  for (const c of Object.keys(manifest.classes)) {
    const o = document.createElement('option');
    o.value = c;
    o.textContent = `${CLASS_NAMES[c] || c} — ${manifest.classes[c].topSpeed} km/h`;
    cls.appendChild(o);
  }
  cls.value = sel.cls;
  const wx = $('weather');
  wx.innerHTML = '';
  for (const [label, preset] of Object.entries(manifest.weather || {})) {
    const o = document.createElement('option');
    o.value = preset;
    o.textContent = label;
    wx.appendChild(o);
  }
  cls.onchange = () => { sel.cls = cls.value; sel.skin = 0; refreshSel(); };
  for (const id of ['opp', 'laps']) {
    $(id).oninput = () => { $(id + '-v').textContent = $(id).value; };
  }
  refreshSel();
}

function refreshSel() {
  for (const b of $('tracks').children) b.classList.toggle('sel', b.dataset.v === sel.track);
  const skins = $('skins');
  skins.innerHTML = '';
  manifest.classes[sel.cls].skins.forEach((s, i) => {
    const b = document.createElement('button');
    b.textContent = i + 1;
    b.title = s;
    b.classList.toggle('sel', i === sel.skin);
    b.onclick = () => { sel.skin = i; refreshSel(); };
    skins.appendChild(b);
  });
}

function show(id) {
  for (const p of ['menu', 'pause', 'results']) $(p).classList.toggle('hidden', p !== id);
  const racing = id === null;
  $('hud').classList.toggle('hidden', !racing && id !== 'pause');
  $('touch').classList.toggle('hidden', !(racing && ('ontouchstart' in window)));
}

async function startRace(opts) {
  Audio.init();
  if (Audio.ctx && Audio.ctx.state === 'suspended') Audio.ctx.resume();
  $('go').disabled = true;
  try {
    const music = manifest.music[2 + Math.floor(Math.random() * (manifest.music.length - 2))];
    const files = [...manifest.common, ...manifest.tracks[opts.track], ...Object.values(manifest.sounds), music];
    setStatus('Loading…', 0);
    await fetchFiles(files, (d, n) => setStatus(`Loading ${manifest.trackNames[opts.track]}… ${d}/${n}`, d / n));
    setStatus('Building track…');
    await new Promise((r) => setTimeout(r, 20));
    const skins = manifest.classes[opts.cls].skins;
    const order = [skins[opts.skin], ...skins.filter((_, i) => i !== opts.skin)];
    const top = String(manifest.classes[opts.cls].topSpeed || 50);
    const ok = Module.ccall('bsr_start', 'number', ['string', 'string', 'string', 'number', 'number', 'string', 'string'],
      [opts.track, opts.cls, order.join(','), opts.opponents, opts.laps, top, opts.weather || 'sunny_bluesky_summer']);
    if (!ok) throw new Error('engine failed to load track');
    lastRace = opts;
    setStatus('');
    show(null);
    $('canvas').focus();
    Audio.startRace(opts.cls);
    Audio.playMusic(music);
    if (Audio.ctx) Audio.play('FX_Comment_racestart_in_3_seconds_A_MW', 0.9);
  } catch (e) {
    console.error(e);
    setStatus('Error: ' + e.message);
    if (params.get('auto')) fetch('/log', { method: 'POST', body: 'ERROR ' + e.message }).catch(() => {});
  }
  $('go').disabled = false;
}

function fmtTime(t) {
  if (!t || t <= 0) return '-:--.--';
  const m = Math.floor(t / 60), s = t - m * 60;
  return `${m}:${s.toFixed(2).padStart(5, '0')}`;
}

function showResults() {
  const res = JSON.parse(Module.ccall('bsr_results', 'string', [], []));
  const rows = res.map((r) => `<tr class="${r.player ? 'me' : ''}"><td>${r.place}</td><td>${r.player ? 'You' : 'CPU'}</td>` +
    `<td>${r.finished ? fmtTime(r.time) : 'running'}</td><td>${fmtTime(r.best)}</td></tr>`).join('');
  $('res-table').innerHTML = '<tr><th>#</th><th>Driver</th><th>Time</th><th>Best lap</th></tr>' + rows;
  show('results');
}

// ------------------------------------------------------------------ engine callbacks
let msgTimer = 0;
function flash(text, ms = 1500) {
  $('h-msg').textContent = text;
  clearTimeout(msgTimer);
  msgTimer = setTimeout(() => { $('h-msg').textContent = ''; }, ms);
}

var Module = {
  canvas: document.getElementById('canvas'),
  print: (t) => { console.log(t); if (params.get('auto')) fetch('/log', { method: 'POST', body: t }).catch(() => {}); },
  printErr: (t) => { console.warn(t); if (params.get('auto')) fetch('/log', { method: 'POST', body: t }).catch(() => {}); },
  onHud(json) {
    const h = JSON.parse(json);
    hudState = h;
    $('h-place').textContent = h.place;
    $('h-racers').textContent = h.racers;
    $('h-lap').textContent = h.lap;
    $('h-laps').textContent = h.laps;
    $('h-time').textContent = fmtTime(h.time);
    $('h-laptime').textContent = fmtTime(h.lapTime);
    $('h-best').textContent = fmtTime(h.best);
    $('h-speed').textContent = Math.round(h.speed);
    $('h-countdown').textContent = h.countdown > 0 ? h.countdown : '';
  },
  onAudio(rpm, thr, skid, impact, speed) { Audio.update(rpm, thr, skid, impact, speed); },
  onEvent(name) {
    if (name === 'beep') Audio.play('menu_updown', 0.8);
    else if (name === 'go') { Audio.play('menu_action', 1); flash('GO!', 900); }
    else if (name === 'lap') flash('LAP ' + hudState.lap);
    else if (name === 'lastlap') flash('FINAL LAP');
    else if (name === 'finish') {
      Audio.play('FX_horn_finish_22_mo_A', 0.9);
      const first = hudState.place === 1;
      flash(first ? 'YOU WIN!' : 'FINISHED P' + hudState.place, 3000);
      setTimeout(() => Audio.play(first ? 'FX_Comment_finish_number_1_A_MW' : 'FX_Comment_race_over_D_MW', 0.9), 900);
      setTimeout(showResults, 3500);
    } else if (name === 'paused') show('pause');
    else if (name === 'resumed') show(null);
  },
  async onRuntimeInitialized() {
    if (params.get('auto')) fetch('/log', { method: 'POST', body: 'runtime ready' }).catch(() => {});
    if (!Module.ccall('bsr_init', 'number', [], [])) {
      setStatus('WebGL 2 is required.');
      if (params.get('auto')) fetch('/log', { method: 'POST', body: 'ERROR no webgl2' }).catch(() => {});
      return;
    }
    try {
      manifest = await (await fetch('data/manifest.json')).json();
    } catch (e) {
      setStatus('data/manifest.json not found — run tools/pack.py first.');
      return;
    }
    buildMenu();
    setStatus('');
    if (params.get('auto')) autoTest();
  },
};

// ------------------------------------------------------------------ UI wiring
$('go').onclick = () => startRace({ track: sel.track, cls: sel.cls, skin: sel.skin,
  opponents: +$('opp').value, laps: +$('laps').value, weather: $('weather').value });
$('resume').onclick = () => { Module.ccall('bsr_set_paused', null, ['number'], [0]); show(null); $('canvas').focus(); };
$('restart').onclick = () => lastRace && startRace(lastRace);
$('quit').onclick = $('tomenu').onclick = () => {
  Module.ccall('bsr_stop', null, [], []);
  Audio.stopRace(); Audio.stopMusic();
  show('menu');
};
$('again').onclick = () => lastRace && startRace(lastRace);
window.addEventListener('keydown', (e) => {
  if (e.code === 'Escape' && $('menu').classList.contains('hidden') && $('results').classList.contains('hidden')) {
    const paused = !$('pause').classList.contains('hidden');
    Module.ccall('bsr_set_paused', null, ['number'], [paused ? 0 : 1]);
    show(paused ? null : 'pause');
  }
});
document.addEventListener('visibilitychange', () => {
  if (document.hidden && $('menu').classList.contains('hidden') && $('pause').classList.contains('hidden') &&
      $('results').classList.contains('hidden')) {
    Module.ccall('bsr_set_paused', null, ['number'], [1]);
    show('pause');
  }
});

// Touch controls: steering pad + gas/brake buttons.
(function touch() {
  const pad = $('t-steer'), knob = $('t-knob');
  let steer = 0, gas = 0, brake = 0;
  const send = () => Module.ccall && Module.ccall('bsr_touch', null, ['number', 'number', 'number'], [steer, gas, brake]);
  const steerFrom = (e) => {
    const r = pad.getBoundingClientRect();
    const t = [...e.touches].find((t) => t.clientX < r.right + 40) || e.touches[0];
    if (!t) return;
    steer = Math.max(-1, Math.min(1, ((t.clientX - r.left) / r.width - 0.5) * 2.4));
    knob.style.left = (r.width / 2 - 30 + steer * (r.width / 2 - 30)) + 'px';
    send();
  };
  pad.addEventListener('touchstart', (e) => { e.preventDefault(); steerFrom(e); });
  pad.addEventListener('touchmove', (e) => { e.preventDefault(); steerFrom(e); });
  pad.addEventListener('touchend', (e) => { e.preventDefault(); steer = 0; knob.style.left = '55px'; send(); });
  const btn = (id, set) => {
    const el = $(id);
    el.addEventListener('touchstart', (e) => { e.preventDefault(); set(1); send(); });
    el.addEventListener('touchend', (e) => { e.preventDefault(); set(0); send(); });
  };
  btn('t-gas', (v) => { gas = v; });
  btn('t-brake', (v) => { brake = v; });
})();

// ------------------------------------------------------------------ automated smoke test (?auto=track&shot=1)
async function autoTest() {
  const track = params.get('auto');
  const cls = params.get('cls') || 'pro_std';
  $('music').checked = false;
  await startRace({ track, cls, skin: 0, opponents: +(params.get('opp') || 5), laps: 3 });
  const shots = (params.get('times') || '0.5,6,14').split(',').map(Number);
  if (params.get('drive')) Module.ccall('bsr_touch', null, ['number', 'number', 'number'], [0, 1, 0]);
  const t0 = performance.now();
  for (const [i, t] of shots.entries()) {
    await new Promise((r) => setTimeout(r, Math.max(0, t * 1000 - (performance.now() - t0))));
    if (params.get('cam')) Module.ccall('bsr_camera', null, [], []);
    const url = $('canvas').toDataURL('image/png');
    await fetch('/shot?n=' + i + '&info=' + encodeURIComponent(JSON.stringify(hudState)), { method: 'POST', body: url }).catch(() => {});
  }
  await fetch('/done', { method: 'POST' }).catch(() => {});
}
