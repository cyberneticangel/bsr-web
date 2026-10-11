// Front-end for the Big Scale Racing WebAssembly build: menu, data loading, HUD, audio, touch input.
'use strict';

const KEY = new Uint8Array([0x47, 0xc3, 0xf5, 0x12, 0x38, 0xe9, 0xb5, 0x91, 0x25, 0x63, 0x06, 0xd9, 0xaa, 0x6f, 0x3a, 0x73]);
const CLASS_NAMES = {
  junior_std: 'Junior', junior_hop: 'Junior Hop-up', trained_std: 'Trained', trained_hop: 'Trained Hop-up',
  skilled_std: 'Skilled', skilled_hop: 'Skilled Hop-up', expert_std: 'Expert', expert_hop: 'Expert Hop-up',
  pro_std: 'Pro', pro_hop: 'Pro Hop-up', Monster: 'Monster Truck',
};
// Skin n of every class is team n (Languages/English.bin team1..team12; the logos on the cars match).
const TEAMS = ['Solar', 'Killroy', 'YinYang', 'daBomb', 'Eyeball', 'Nukem', 'Alien', 'HotLips', 'Bulls', 'Skullz',
  'Electron', 'Flash'];
const className = (c) => CLASS_NAMES[Object.keys(CLASS_NAMES).find((k) => k.toLowerCase() === String(c).toLowerCase())] || c;
const params = new URLSearchParams(location.search);
const $ = (id) => document.getElementById(id);
const MAX_RACERS = 8;  // grid slots on every track

// Player preferences, kept in this browser.
const Settings = {
  musicMode: 'stereo',  // stereo | localized (from the trackside speakers) | off
  genres: { surf: true, rock: true, dance: true },
  load() {
    try { Object.assign(this, JSON.parse(localStorage.getItem('bsr-settings')) || {}); } catch (e) {}
  },
  save() {
    const { musicMode, genres, hudScale, hudStyle } = this;
    try { localStorage.setItem('bsr-settings', JSON.stringify({ musicMode, genres, hudScale, hudStyle })); } catch (e) {}
  },
};
Settings.load();

let manifest = null;
const loaded = new Set();
const sel = { mode: 'single', track: 'baanbreker', cls: 'pro_std', skin: 0, skin2: 1, raceType: 'cup', lenMode: 'laps', ghost: 1 };
const RACE_MINUTES = [1, 2, 3, 5, 10, 20];
// Menu weather, as in the original. Each track's <track>_weather_<cat>.bin lists the presets a category picks from.
const WEATHER_CATS = { sunny: 'Sunny', cloudy: 'Cloudy', sunset: 'Sunset', wet: 'Wet', rain: 'Rain', random: 'Random' };

// Resolves the menu choice into { cat, preset } for a track (random category, then a preset by the track's weights).
function pickWeather(track, choice) {
  const cats = (manifest.weather || {})[track] || {};
  if (choice && !cats[choice] && choice !== 'random') return { cat: '', preset: choice };  // a preset name
  const names = Object.keys(cats);
  if (!names.length) return { cat: '', preset: 'sunny_bluesky_summer' };
  const cat = cats[choice] ? choice : names[Math.floor(Math.random() * names.length)];
  const { presets, weights } = cats[cat];
  const total = weights.reduce((a, b) => a + Math.max(b, 0), 0);
  let x = Math.random() * total, i = 0;
  while (i < presets.length - 1 && (x -= Math.max(weights[i], 0)) > 0) i++;
  return { cat, preset: presets[i] };
}
let lastRace = null;
let race = null;  // current race: { opts, count, local, names, finished }
const hudStates = [{}, {}];
let hudState = hudStates[0];

// Sets an AudioParam triple (positionX/Y/Z) directly, or through the older setPosition().
function setParam(node, name, x, y, z) {
  if (node[name + 'X']) { node[name + 'X'].value = x; node[name + 'Y'].value = y; node[name + 'Z'].value = z; }
  else if (name === 'position' && node.setPosition) node.setPosition(x, y, z);
}

function xorInPlace(u8, start = 0) {
  for (let i = start; i < u8.length; i++) u8[i] ^= KEY[(i - start) & 15];
  return u8;
}

// ------------------------------------------------------------------ audio
const Audio = {
  ctx: null, buffers: {}, cars: [], crowd: null, master: null, music: null, musicEl: null,
  lastImpact: [0, 0], enabled: true,
  vol: { comments: 0.9, music: 0.45 },
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
  // One engine + skid loop per local player (two in split screen).
  startRace(cls, players = 1) {
    this.stopRace();
    if (!this.ctx) return;
    for (let p = 0; p < players; p++) {
      this.cars.push({
        engine: this.loop(cls.endsWith('hop') ? 'FX_Engine_st_22_ZENOAH_A' : 'FX_Engine_st_22_serpent_A', 0),
        skid: this.loop('FX_tire_slip_A', 0),
      });
    }
    this.crowd = this.loop('FX_env_crowd_gen_A_22', 0.12);
  },
  stopRace() {
    const stop = (v) => { if (v) try { v.src.stop(); } catch (e) {} };
    for (const c of this.cars) { stop(c.engine); stop(c.skid); }
    this.cars = [];
    stop(this.crowd);
    this.crowd = null;
    stop(this.rain);
    this.rain = null;
    clearTimeout(this.thunderTimer);
  },
  // Rain loop, and for thunder presets (..._T) thunder at random intervals with a lightning flash.
  weather(preset, cat) {
    const p = String(preset).toLowerCase();
    const rain = cat === 'rain' || /_r_/.test(p) || p.startsWith('rain');
    if (!this.ctx || !rain) return;
    const heavy = /dens|bad|pouring/.test(p);
    this.rain = this.loop(heavy ? 'FX_weather_rain_heavy' : 'FX_weather_rain_steady_A', heavy ? 0.4 : 0.3);
    if (this.crowd) this.crowd.g.gain.value = 0.06;
    if (!p.endsWith('_t')) return;
    const strike = () => {
      if (!race) return;
      const near = Math.random() < 0.35;
      if (near) lightning();
      setTimeout(() => this.play(near ? (Math.random() < 0.5 ? 'FX_weather_thunder_close_A' : 'FX_weather_thunder_nearby')
        : (Math.random() < 0.5 ? 'FX_weather_thunder_far_A' : 'FX_weather_thunder_very_far_A'), near ? 0.8 : 0.5),
      near ? 150 : 0);
      this.thunderTimer = setTimeout(strike, 9000 + Math.random() * 22000);
    };
    this.thunderTimer = setTimeout(strike, 4000 + Math.random() * 8000);
  },
  // Called every frame. The loops are smoothed here and written to .value: scheduling a setTargetAtTime per frame
  // grows the AudioParam event timeline, which some engines (Firefox) don't prune during a long race.
  update(rpm, throttle, skid, impact, speed, p = 0) {
    if (!this.ctx) return;
    const car = this.cars[p] || {};
    const now = performance.now();
    const k = 1 - Math.exp(-Math.min(0.1, (now - (car.last || now)) / 1000) / 0.05);  // 50 ms time constant
    car.last = now;
    const ease = (param, target) => { param.value += (target - param.value) * k; };
    if (car.engine) {
      ease(car.engine.src.playbackRate, 0.55 + rpm * 1.5);
      ease(car.engine.g.gain, 0.18 + throttle * 0.22);
    }
    if (car.skid) ease(car.skid.g.gain, Math.min(0.5, skid * 0.6) * (speed > 1.5 ? 1 : 0));
    if (impact > 1.2 && performance.now() - this.lastImpact[p] > 180) {
      this.lastImpact[p] = performance.now();
      this.play(impact > 3 ? 'FX_auto_botst_c' : 'AU_FX_botst_alg_A', Math.min(1, impact / 5), 0.9 + Math.random() * 0.2);
    }
  },
  // Background music from an MP3 (.fs3 = MP3 XORed with the key), fetched on demand. One track plays at a time.
  async playMusic(path, loop = true) {
    this.stopMusic();
    if (!path || Settings.musicMode === 'off') return;
    const token = this.musicToken = {};
    try {
      await fetchFiles([path], () => {});
      if (token !== this.musicToken) return;  // something else started meanwhile
      const u8 = xorInPlace(Module.FS.readFile('/data/' + path).slice());
      const url = URL.createObjectURL(new Blob([u8], { type: 'audio/mpeg' }));
      const el = new window.Audio(url);
      el.loop = loop;
      el.volume = this.vol.music;
      el.play().catch(() => {});
      this.musicEl = el;
      this.musicPath = path;
    } catch (e) {}
  },
  // Localized music: the song's mono WAV played from the track's loudspeakers, positioned in 3D around the camera.
  playLocalized(wavPath, speakers) {
    this.stopMusic();
    if (!this.ctx || !wavPath) return;
    let buf = null;
    try { buf = this.decodeFSW(Module.FS.readFile('/data/' + wavPath)); } catch (e) {}
    if (!buf) return;
    const src = this.ctx.createBufferSource();
    src.buffer = buf;
    src.loop = true;
    const g = this.ctx.createGain();
    g.gain.value = this.vol.music * 2.2;  // far away most of the time
    src.connect(g);
    const panners = speakers.map(([x, y, z]) => {
      const pn = this.ctx.createPanner();
      pn.panningModel = 'HRTF';
      pn.distanceModel = 'inverse';
      pn.refDistance = 6;
      pn.rolloffFactor = 1;
      setParam(pn, 'position', x, y, z);
      g.connect(pn).connect(this.master);
      return pn;
    });
    if (!panners.length) g.connect(this.master);  // no speakers on this track: plain mono
    src.start();
    this.localized = { src, g, panners };
  },
  setListener(x, y, z, fx, fy, fz) {
    if (!this.ctx || !this.localized) return;
    const l = this.ctx.listener;
    setParam(l, 'position', x, y, z);
    if (l.forwardX) {
      l.forwardX.value = fx; l.forwardY.value = fy; l.forwardZ.value = fz;
      l.upX.value = 0; l.upY.value = 0; l.upZ.value = 1;
    } else if (l.setOrientation) {
      l.setOrientation(fx, fy, fz, 0, 0, 1);
    }
  },
  stopMusic() {
    this.musicToken = null;
    if (this.musicEl) { this.musicEl.pause(); URL.revokeObjectURL(this.musicEl.src); this.musicEl = null; }
    this.musicPath = null;
    if (this.localized) {
      try { this.localized.src.stop(); } catch (e) {}
      this.localized.g.disconnect();
      this.localized = null;
    }
  },
  // The menu theme (Music.ini "Menus"), also in the online lobby. Keeps playing if it already is.
  menuMusic() {
    if (!manifest || this.musicPath === manifest.music.menu) return;
    this.playMusic(manifest.music.menu);
  },
};

// The race commentator. The disc has three voices (IJB, MW, PS); one is picked per race. Lines are the
// FX_Comment_<line>_<variant>_<voice> sounds, e.g. FX_Comment_new_lap_record_A_MW.
const Announcer = {
  voices: ['MW', 'IJB', 'PS'],
  last: {},
  pickVoice() { return this.voices[Math.floor(Math.random() * this.voices.length)]; },
  // line: e.g. 'race_over'; any variant in the race's voice, else in another voice. cooldown: seconds per line.
  say(line, delay = 0, cooldown = 0) {
    if (!manifest || !Audio.ctx) return;
    const now = performance.now() / 1000;
    if (cooldown && now - (this.last[line] || -1e9) < cooldown) return;
    this.last[line] = now;
    const pre = ('FX_Comment_' + line + '_').toLowerCase();
    const all = Object.keys(manifest.sounds).filter((k) => k.toLowerCase().startsWith(pre));
    const voice = (race && race.voice) || this.voices[0];
    const mine = all.filter((k) => k.endsWith('_' + voice));
    const pool = mine.length ? mine : all;
    if (!pool.length) return;
    const name = pool[Math.floor(Math.random() * pool.length)];
    if (delay > 0) setTimeout(() => Audio.play(name, Audio.vol.comments), delay * 1000);
    else Audio.play(name, Audio.vol.comments);
  },
};

// Lap records per track and class, kept in this browser.
const Records = {
  key: 'bsr-records',
  all() { try { return JSON.parse(localStorage.getItem(this.key)) || {}; } catch (e) { return {}; } },
  get(track, cls) { return this.all()[track + '|' + cls] || 0; },
  // Returns true for a new record (and stores it).
  submit(track, cls, t) {
    const all = this.all(), k = track + '|' + cls;
    if (all[k] && all[k] <= t) return false;
    all[k] = t;
    try { localStorage.setItem(this.key, JSON.stringify(all)); } catch (e) {}
    return true;
  },
};

// ------------------------------------------------------------------ HUD
// .fsp texture -> RGBA image data URL (header, XOR, TGA-style RLE, rows bottom-up; see FORMATS.md).
function fspToURL(u8) {
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const w = dv.getUint32(0, true), h = dv.getUint32(4, true), fmt = dv.getUint32(8, true), size = dv.getUint32(16, true);
  const bpp = { 2: 2, 4: 2, 5: 2, 7: 3, 11: 4 }[fmt];
  if (!bpp) return '';
  const src = xorInPlace(u8.slice(20, 20 + size));
  const px = new Uint8Array(w * h * bpp);
  for (let i = 0, o = 0; i < src.length && o < px.length;) {
    const c = src[i++], n = (c & 0x7f) + 1;
    if (c & 0x80) { for (let k = 0; k < n; k++) px.set(src.subarray(i, i + bpp), o + k * bpp); i += bpp; }
    else { px.set(src.subarray(i, i + n * bpp), o); i += n * bpp; }
    o += n * bpp;
  }
  const canvas = document.createElement('canvas');
  canvas.width = w;
  canvas.height = h;
  const g = canvas.getContext('2d');
  if (!g) return '';
  const img = g.createImageData(w, h);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const s = ((h - 1 - y) * w + x) * bpp, d = (y * w + x) * 4;
      if (bpp >= 3) { img.data[d] = px[s + 2]; img.data[d + 1] = px[s + 1]; img.data[d + 2] = px[s]; img.data[d + 3] = bpp === 4 ? px[s + 3] : 255; }
      else {
        const v = px[s] | px[s + 1] << 8;
        if (fmt === 5) { img.data[d] = (v >> 8 & 15) * 17; img.data[d + 1] = (v >> 4 & 15) * 17; img.data[d + 2] = (v & 15) * 17; img.data[d + 3] = (v >> 12) * 17; }
        else { img.data[d] = (v >> 11) * 8; img.data[d + 1] = (v >> 5 & 63) * 4; img.data[d + 2] = (v & 31) * 8; img.data[d + 3] = 255; }
      }
    }
  g.putImageData(img, 0, 0);
  return canvas.toDataURL('image/png');
}

const Hud = {
  // Countdown glyphs in Font_54321 (256x128): x, y, w, h.
  digits: { 5: [4, 0, 46, 62], 4: [64, 0, 48, 62], 3: [124, 0, 46, 60], 2: [4, 60, 46, 53], 1: [64, 60, 36, 53],
    go: [108, 58, 114, 57] },
  shown: ['', ''],
  // Builds both player views from the template (ids "$x" -> "h-x" / "h2-x").
  build() {
    const tpl = $('hud-tpl').innerHTML;
    for (const v of document.querySelectorAll('.hud-view')) v.innerHTML = tpl.replace(/\$(\w+)/g, v.dataset.pre + '$1');
    for (const el of document.querySelectorAll('#hud [data-img]')) el.style.setProperty('--img', `var(--img-${el.dataset.img})`);
    this.scale(Settings.hudScale || 1);
  },
  async loadImages() {
    const files = Object.values(manifest.hud || {});
    await fetchFiles(files, () => {});
    for (const [name, path] of Object.entries(manifest.hud || {})) {
      const url = fspToURL(Module.FS.readFile('/data/' + path));
      if (url) $('hud').style.setProperty('--img-' + name, `url(${url})`);
    }
  },
  scale(k) { $('hud').style.setProperty('--hud', k); },
  // The original's countdown digits; key: 1..5 or 'go'.
  countdown(p, key) {
    const pre = p ? 'h2-' : 'h-';
    if (this.shown[p] === key) return;
    this.shown[p] = key;
    const el = $(pre + 'countdown');
    el.innerHTML = '';
    const d = this.digits[key];
    if (!d) return;
    const i = document.createElement('i');
    const k = 6;  // em per 64 px of the sheet
    const u = (v) => (v / 64 * k).toFixed(3) + 'em';
    Object.assign(i.style, { width: u(d[2]), height: u(d[3]), backgroundPosition: `-${u(d[0])} -${u(d[1])}`,
      backgroundSize: `${u(256)} ${u(128)}`, fontSize: '1em' });
    el.appendChild(i);
    if (key === 'go') setTimeout(() => { if (this.shown[p] === 'go') this.countdown(p, ''); }, 900);
  },
};
Hud.build();

// A lightning flash over the whole screen.
function lightning() {
  const el = $('flash');
  el.classList.remove('on');
  void el.offsetWidth;  // restart the animation
  el.classList.add('on');
}

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
  for (const [v, label] of Object.entries(WEATHER_CATS)) {
    const o = document.createElement('option');
    o.value = v;
    o.textContent = label;
    wx.appendChild(o);
  }
  cls.onchange = () => { sel.cls = cls.value; sel.skin = 0; sel.skin2 = 1; refreshSel(); Net.sendProfile(); };
  for (const id of ['opp', 'laps']) {
    $(id).oninput = () => { $(id + '-v').textContent = $(id).value; Net.pushSettings(); };
  }
  wx.onchange = $('minutes').onchange = () => Net.pushSettings();
  for (const b of $('modes').children) b.onclick = () => setMode(b.dataset.v);
  const choice = (id, key, conv = (v) => v) => {
    for (const b of $(id).children) b.onclick = () => { sel[key] = conv(b.dataset.v); refreshSel(); Audio.play('menu_leftright', 0.5); };
  };
  choice('racetype', 'raceType');
  choice('lenmode', 'lenMode');
  choice('ghost', 'ghost', Number);
  for (const b of $('musicmode').children) b.onclick = () => {
    Settings.musicMode = b.dataset.v;
    Settings.save();
    refreshSel();
    if (Settings.musicMode === 'off') Audio.stopMusic(); else Audio.menuMusic();
  };
  $('hudscale').value = Math.round((Settings.hudScale || 1) * 100);
  $('hudscale-v').textContent = $('hudscale').value + '%';
  $('hudscale').oninput = () => {
    Settings.hudScale = +$('hudscale').value / 100;
    $('hudscale-v').textContent = $('hudscale').value + '%';
    Hud.scale(Settings.hudScale);
    Settings.save();
  };
  for (const b of $('genres').children) b.onclick = () => {
    Settings.genres[b.dataset.v] = !Settings.genres[b.dataset.v];
    Settings.save();
    refreshSel();
  };
  refreshSel();
}

// Car previews for the skin picker, rendered by the engine (bsr_preview) once the class's files are loaded.
const Previews = {
  urls: {},       // 'cls|skin index' -> data URL
  loading: {},    // cls -> Promise
  W: 192, H: 120,
  // Resolves once every skin of the class has a preview.
  load(cls) {
    if (this.loading[cls]) return this.loading[cls];
    return (this.loading[cls] = (async () => {
      await fetchFiles(manifest.classes[cls].files || [], () => {});
      const canvas = document.createElement('canvas');
      canvas.width = this.W;
      canvas.height = this.H;
      const g = canvas.getContext('2d');
      manifest.classes[cls].skins.forEach((skin, i) => {
        const ptr = Module.ccall('bsr_preview', 'number', ['string', 'string', 'number', 'number'], [cls, skin, this.W, this.H]);
        if (!ptr) return;
        const img = new ImageData(new Uint8ClampedArray(Module.HEAPU8.buffer, ptr, this.W * this.H * 4).slice(), this.W, this.H);
        g.clearRect(0, 0, this.W, this.H);
        g.putImageData(img, 0, 0);
        // GL rows are bottom-up: flip by drawing the canvas onto itself mirrored.
        const flipped = document.createElement('canvas');
        flipped.width = this.W;
        flipped.height = this.H;
        const fg = flipped.getContext('2d');
        fg.scale(1, -1);
        fg.drawImage(canvas, 0, -this.H);
        this.urls[cls + '|' + i] = flipped.toDataURL('image/png');
      });
    })().catch((e) => { console.warn('previews', e); delete this.loading[cls]; }));
  },
  url(cls, i) { return this.urls[cls + '|' + i] || ''; },
};

function skinButtons(container, current, onPick) {
  container.innerHTML = '';
  const cls = sel.cls;
  manifest.classes[cls].skins.forEach((s, i) => {
    const b = document.createElement('button');
    b.className = 'skin';
    b.title = `${TEAMS[i] || 'Team ' + (i + 1)} (${s})`;
    const img = document.createElement('img');
    img.alt = '';
    img.src = Previews.url(cls, i) || 'data:,';
    const cap = document.createElement('span');
    cap.textContent = TEAMS[i] || i + 1;
    b.append(img, cap);
    b.classList.toggle('sel', i === current);
    b.onclick = () => { onPick(i); refreshSel(); Audio.play('menu_leftright', 0.5); };
    container.appendChild(b);
  });
  if (!Previews.url(cls, 0))
    Previews.load(cls).then(() => {
      if (sel.cls !== cls) return;
      container.querySelectorAll('img').forEach((img, i) => { img.src = Previews.url(cls, i) || 'data:,'; });
    });
}

function refreshSel() {
  for (const b of $('tracks').children) b.classList.toggle('sel', b.dataset.v === sel.track);
  for (const b of $('modes').children) b.classList.toggle('sel', b.dataset.v === sel.mode);
  const nSkins = manifest.classes[sel.cls].skins.length;
  sel.skin %= nSkins;
  sel.skin2 %= nSkins;
  skinButtons($('skins'), sel.skin, (i) => { sel.skin = i; Net.sendProfile(); });
  skinButtons($('skins2'), sel.skin2, (i) => { sel.skin2 = i; });
  const split = sel.mode === 'split', online = sel.mode === 'online', practice = sel.mode === 'practice';
  for (const b of $('racetype').children) b.classList.toggle('sel', b.dataset.v === sel.raceType);
  for (const b of $('lenmode').children) b.classList.toggle('sel', b.dataset.v === sel.lenMode);
  for (const b of $('ghost').children) b.classList.toggle('sel', +b.dataset.v === sel.ghost);
  $('len-laps').classList.toggle('hidden', sel.lenMode !== 'laps');
  $('minutes').classList.toggle('hidden', sel.lenMode !== 'time');
  for (const id of ['row-racetype', 'row-opp', 'row-length']) $(id).classList.toggle('hidden', practice);
  $('row-ghost').classList.toggle('hidden', !practice);
  for (const b of $('musicmode').children) b.classList.toggle('sel', b.dataset.v === Settings.musicMode);
  for (const b of $('genres').children) b.classList.toggle('sel', !!Settings.genres[b.dataset.v]);
  $('row-genres').classList.toggle('hidden', Settings.musicMode === 'off');
  $('row-skin2').classList.toggle('hidden', !split);
  $('skin-label').textContent = split ? 'P1 skin' : 'Skin';
  $('opp-label').textContent = online ? 'AI cars' : 'Opponents';
  $('opp').max = split ? MAX_RACERS - 2 : MAX_RACERS - 1;
  if (+$('opp').value > +$('opp').max) $('opp').value = $('opp').max;
  $('opp-v').textContent = $('opp').value;
  $('online').classList.toggle('hidden', !online);
  // Online guests see the host's settings but can't change them; they only pick their own skin.
  const guest = online && !Net.isHost();
  for (const b of $('tracks').children) b.disabled = guest;
  for (const id of ['racetype', 'lenmode']) for (const b of $(id).children) b.disabled = guest;
  for (const id of ['weather', 'opp', 'laps', 'minutes']) $(id).disabled = guest;
  $('cls').disabled = guest && sel.raceType !== 'open';  // open races: everyone brings their own class
  const go = $('go');
  if (!online) { go.textContent = 'RACE'; go.disabled = false; }
  else if (!Net.connected()) { go.textContent = 'JOIN OR CREATE A ROOM'; go.disabled = true; }
  else if (guest) { go.textContent = 'WAITING FOR HOST'; go.disabled = true; }
  else { go.textContent = 'START RACE'; go.disabled = false; }
  Net.pushSettings();
}

// { laps, minutes }: minutes > 0 is a timed race.
function raceLength() {
  return sel.lenMode === 'time' ? { laps: 0, minutes: +$('minutes').value } : { laps: +$('laps').value, minutes: 0 };
}

function setMode(mode) {
  if (sel.mode === 'online' && mode !== 'online') Net.close();
  sel.mode = mode;
  refreshSel();
  Net.renderLobby();
}

function show(id) {
  for (const p of ['menu', 'pause', 'results']) $(p).classList.toggle('hidden', p !== id);
  const racing = id === null;
  $('hud').classList.toggle('hidden', !racing && id !== 'pause');
  const split = !!race && race.local > 1;
  $('hud').classList.toggle('split', split);
  $('touch').classList.toggle('hidden', !(racing && !split && ('ontouchstart' in window)));
  if (id !== 'results') clearInterval(showResults.timer);
  if (id === 'menu') Audio.menuMusic();
  if (id === 'results' && manifest) Audio.playMusic(manifest.music.finish);
  if (id === 'pause') {  // online the race can't be paused or restarted, only left
    const net = !!race && !!race.opts.online;
    $('pause-title').textContent = net ? 'RACE MENU' : 'PAUSED';
    $('restart').classList.toggle('hidden', net);
    $('quit').textContent = net ? 'LEAVE RACE' : 'QUIT TO MENU';
  }
}

// Opponents for an open race come from every class (the Monster truck only races Monster trucks).
function randomClass(playerCls) {
  const all = Object.keys(manifest.classes).filter((c) => c !== 'Monster');
  return playerCls === 'Monster' ? 'Monster' : all[Math.floor(Math.random() * all.length)];
}

// Grid for a race on this machine: local players first (they start at the back), then AI cars.
function localLineup(opts) {
  const nSkins = manifest.classes[opts.cls].skins.length;
  const humans = opts.split ? [opts.skin, opts.skin2 ?? (opts.skin + 1) % nSkins] : [opts.skin];
  const free = [...Array(nSkins).keys()].filter((i) => !humans.includes(i));
  if (!free.length) free.push(...Array(nSkins).keys());
  const racers = humans.map((skin, p) => ({ role: String(p), cls: opts.cls, skin, name: opts.split ? 'P' + (p + 1) : 'You' }));
  const ai = opts.practice ? 0 : Math.max(0, Math.min(opts.opponents, MAX_RACERS - humans.length));
  for (let i = 0; i < ai; i++) {
    const cls = opts.raceType === 'open' ? randomClass(opts.cls) : opts.cls;
    const skin = cls === opts.cls ? free[i % free.length] : Math.floor(Math.random() * manifest.classes[cls].skins.length);
    racers.push({ role: 'a', cls, skin, name: 'CPU' });
  }
  return racers;
}

// The engine's race description (see parseRaceConfig in src/game.cpp).
function raceConfig(opts) {
  const cars = opts.racers.map((r) => {
    const c = manifest.classes[r.cls] || manifest.classes[opts.cls];
    return [r.cls, c.skins[r.skin % c.skins.length], c.topSpeed || 50, r.role].join(',');
  }).join('|');
  return [`track=${opts.track}`, `weather=${opts.weather || 'sunny_bluesky_summer'}`, `wxcat=${opts.weatherCat || ''}`, `laps=${opts.laps || 3}`, `time=${(opts.minutes || 0) * 60}`,
    `practice=${opts.practice ? 1 : 0}`, `ghost=${opts.practice ? opts.ghost || 0 : 0}`, `cars=${cars}`].join(';');
}

// A random on-track song: { mp3, wav }.
function pickSong() {
  let genres = ['surf', 'rock', 'dance'].filter((g) => Settings.genres[g]);
  if (!genres.length) genres = ['surf', 'rock', 'dance'];
  const songs = genres.flatMap((g) => manifest.music[g] || []);
  return songs[Math.floor(Math.random() * songs.length)] || null;
}

// opts: { track, cls, laps, minutes?, weather, practice?, ghost?, raceType?, song?, online?,
//         racers: [{ role, cls, skin, name }] }
//       or without racers: { ..., skin, skin2?, split?, opponents } and the grid is made here.
async function startRace(opts) {
  if (!opts.racers) opts = { ...opts, racers: localLineup(opts) };
  if (!opts.weatherCat) {  // menu choice -> concrete preset (online, the host resolves it for everyone)
    const w = pickWeather(opts.track, opts.weather);
    opts = { ...opts, weather: w.preset, weatherCat: w.cat };
  }
  Audio.init();
  if (Audio.ctx && Audio.ctx.state === 'suspended') Audio.ctx.resume();
  $('go').disabled = true;
  let ok = 0;
  try {
    const song = opts.song || pickSong();
    const wxFiles = ((manifest.weather[opts.track] || {})[opts.weatherCat] || {}).files || [];
    const files = [...manifest.common, ...manifest.tracks[opts.track], ...wxFiles, ...Object.values(manifest.sounds)];
    if (song && Settings.musicMode !== 'off') files.push(Settings.musicMode === 'localized' ? song.wav : song.mp3);
    setStatus('Loading…', 0);
    await fetchFiles(files, (d, n) => setStatus(`Loading ${manifest.trackNames[opts.track]}… ${d}/${n}`, d / n));
    setStatus('Building track…');
    await new Promise((r) => setTimeout(r, 20));
    if (opts.online && !Net.stillCurrent(opts)) return 0;  // host started another race while we loaded
    Module.ccall('bsr_set_online', null, ['number'], [opts.online ? 1 : 0]);
    ok = Module.ccall('bsr_start', 'number', ['string'], [raceConfig(opts)]);
    if (!ok) throw new Error('engine failed to load track');
    if (opts.online) Module.ccall('bsr_set_waiting', null, ['number'], [1]);  // until every player has loaded
    const local = opts.racers.slice(0, ok).filter((r) => /^[0-9]$/.test(r.role)).length;
    race = {
      opts, count: ok, local: Math.max(1, local), names: opts.racers.map((r) => r.name), finished: new Set(),
      voice: Announcer.pickVoice(), best: [0, 0],
    };
    for (const h of hudStates) for (const k of Object.keys(h)) delete h[k];
    lastRace = opts;
    setStatus('');
    show(null);
    for (const pre of ['h-', 'h2-']) $(pre + 'msg').textContent = '';
    $('h-keys').textContent = local > 1 ? 'WASD drive · Space handbrake · C camera · R reset · P pause'
      : '↑↓←→ drive · Space handbrake · C camera · R reset' + (opts.online ? ' · Esc menu' : ' · P pause');
    $('h2-keys').textContent = '↑↓←→ drive · Right Shift handbrake · . camera · Backspace reset';
    Hud.shown = ['', ''];
    $('canvas').focus();
    Audio.startRace(opts.cls, race.local);
    Audio.weather(opts.weather, opts.weatherCat);
    if (song && Settings.musicMode === 'localized')
      Audio.playLocalized(song.wav, JSON.parse(Module.ccall('bsr_speakers', 'string', [], [])));
    else if (song) Audio.playMusic(song.mp3);
    else Audio.stopMusic();
    if (!opts.online) Announcer.say('racestart_in_3_seconds');
  } catch (e) {
    ok = 0;
    console.error(e);
    setStatus('Error: ' + e.message);
    if (params.get('auto')) fetch('/log', { method: 'POST', body: 'ERROR ' + e.message }).catch(() => {});
  } finally {
    refreshSel();
  }
  return ok;
}

function stopRace() {
  Module.ccall('bsr_stop', null, [], []);
  Audio.stopRace();
  Audio.stopMusic();
  race = null;
}

function fmtTime(t) {
  if (!t || t <= 0) return '-:--.--';
  const m = Math.floor(t / 60), s = t - m * 60;
  return `${m}:${s.toFixed(2).padStart(5, '0')}`;
}

function showResults() {
  if (!race) return;
  const res = JSON.parse(Module.ccall('bsr_results', 'string', [], []));
  const esc = (t) => String(t).replace(/[&<>"]/g, (c) => `&#${c.charCodeAt(0)};`);
  const open = race.opts.racers.some((r) => r.cls !== race.opts.racers[0].cls);
  const timed = race.opts.minutes > 0;
  const rows = res.map((r) => {
    const name = race.names[r.i] ?? (r.player ? 'You' : 'CPU');
    const time = r.finished ? fmtTime(r.time) : r.gone ? 'left' : 'running';
    return `<tr class="${r.player ? 'me' : ''}"><td>${r.place}</td><td>${esc(name)}</td>` +
      (open ? `<td>${esc(className(r.cls))}</td>` : '') +
      (timed ? `<td>${r.lap}</td>` : '') + `<td>${time}</td><td>${fmtTime(r.best)}</td></tr>`;
  }).join('');
  $('res-table').innerHTML = '<tr><th>#</th><th>Driver</th>' + (open ? '<th>Class</th>' : '') + (timed ? '<th>Laps</th>' : '') +
    '<th>Time</th><th>Best lap</th></tr>' + rows;
  const online = !!race.opts.online;
  $('again').classList.toggle('hidden', online && !Net.isHost());
  $('res-note').textContent = online ? (Net.isHost() ? 'Race again starts a new race for the whole room.' : 'The host starts the next race.') : '';
  $('tomenu').textContent = online ? 'LOBBY' : 'MENU';
  if ($('results').classList.contains('hidden')) show('results');
  // Online, others may still be racing: keep the table live.
  clearInterval(showResults.timer);
  if (online && res.some((r) => !r.finished && !r.gone)) showResults.timer = setInterval(showResults, 1000);
}

// ------------------------------------------------------------------ engine callbacks
function onLapTime(p, t) {
  if (!race || !(t > 0)) return;
  const cls = race.opts.racers.find((r) => r.role === String(p))?.cls || race.opts.cls;
  const before = Records.get(race.opts.track, cls);
  if (Records.submit(race.opts.track, cls, t) && before) {  // the very first lap on a track isn't news
    flash('! - New Track Record - !', 2500, p);
    Announcer.say('new_lap_record', 0.3);
  } else if (race.best[p] && t < race.best[p]) {
    flash('! - Your best lap this race - !', 2000, p);
  }
  race.best[p] = Math.min(race.best[p] || Infinity, t);
}

const msgTimers = [0, 0];
function flash(text, ms = 1500, p) {
  for (const i of p === undefined ? [0, 1] : [p]) {
    const el = $(i ? 'h2-msg' : 'h-msg');
    el.textContent = text;
    clearTimeout(msgTimers[i]);
    msgTimers[i] = setTimeout(() => { el.textContent = ''; }, ms);
  }
}

var Module = {
  canvas: document.getElementById('canvas'),
  print: (t) => { console.log(t); if (params.get('auto')) fetch('/log', { method: 'POST', body: t }).catch(() => {}); },
  printErr: (t) => { console.warn(t); if (params.get('auto')) fetch('/log', { method: 'POST', body: t }).catch(() => {}); },
  onHud(json, p = 0) {
    const h = JSON.parse(json);
    const prev = hudStates[p];
    for (const k of Object.keys(prev)) delete prev[k];
    Object.assign(prev, h);
    const pre = p ? 'h2-' : 'h-';
    $(pre + 'place').textContent = h.place;
    $(pre + 'racers').textContent = h.racers;
    $(pre + 'lap').textContent = h.lap;
    $(pre + 'laps').textContent = h.laps;
    $(pre + 'lapsof').classList.toggle('hidden', !h.laps);  // timed races and practice have no lap count
    $(pre + 'timelabel').textContent = h.timeLeft >= 0 ? 'LEFT' : 'TIME';
    $(pre + 'time').textContent = fmtTime(h.timeLeft >= 0 ? h.timeLeft : h.time);
    $(pre + 'laptime').textContent = fmtTime(h.lapTime);
    $(pre + 'best').textContent = fmtTime(h.best);
    $(pre + 'speed').textContent = Math.round(h.speed);
    $(pre + 'spd').style.width = Math.min(100, h.speed / Math.max(h.top, 1) * 100).toFixed(1) + '%';
    $(pre + 'rev').style.width = Math.min(100, h.rpm * 85).toFixed(1) + '%';
    if (h.countdown > 0 && !h.waiting) Hud.countdown(p, Math.min(5, h.countdown));
    else if (Hud.shown[p] !== 'go') Hud.countdown(p, '');
    if (h.waiting) $(pre + 'msg').textContent = 'Waiting for players…';
    else if ($(pre + 'msg').textContent === 'Waiting for players…') $(pre + 'msg').textContent = '';
  },
  onAudio(rpm, thr, skid, impact, speed, p = 0) { Audio.update(rpm, thr, skid, impact, speed, p); },
  onListener(x, y, z, fx, fy, fz) { Audio.setListener(x, y, z, fx, fy, fz); },
  onEvent(event) {
    const [name, ps, value] = event.split(':');
    const p = ps === undefined || ps === '' ? 0 : Math.max(0, +ps);
    const h = hudStates[p];
    const who = race && race.local > 1 ? `P${p + 1} ` : '';
    if (name === 'beep') Audio.play('menu_updown', 0.8);
    else if (name === 'go') { Audio.play('menu_action', 1); Hud.countdown(0, 'go'); Hud.countdown(1, 'go'); }
    else if (name === 'lap') flash('LAP ' + h.lap, 1500, p);
    else if (name === 'lastlap') flash('FINAL LAP!', 1500, p);
    else if (name === 'laptime') onLapTime(p, +value);
    else if (name === 'missedcp' || name === 'wrongcp') {
      Audio.play('FX_WARNING', 0.7);
      flash(name === 'missedcp' ? 'YOU MISSED A CHECKPOINT!' : 'WRONG CHECKPOINT!', 2000, p);
    } else if (name === 'timeleft') {
      flash(ps === '1' ? '1 Minute left!' : ps + ' Minutes left!', 2500);
      Announcer.say(ps + '_minutes_left_to_race');
    } else if (name === 'timeup') {
      flash('TIME IS UP!', 2500);
      Announcer.say('race_time_is_up');
    } else if (name === 'deadcar') {
      if (+ps < 0) Announcer.say('warning_dead_car', 0, 25);  // someone else's car is stuck or on its roof
    } else if (name === 'finish') {
      Audio.play('FX_horn_finish_22_mo_A', 0.9);
      const first = h.place === 1;
      flash(first ? (who ? who + 'WINS!' : 'YOU WIN!') : who + 'FINISHED P' + h.place, 3000, p);
      if (!race) return;
      race.finished.add(p);
      if (race.finished.size < race.local) return;  // results once every local player is home
      const r = race;
      Announcer.say(first ? 'finish_number_1' : 'race_over', 0.9);
      setTimeout(() => { if (race === r) showResults(); }, 3500);
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
    Hud.loadImages().catch((e) => console.warn('hud', e));
    if (params.get('room')) {  // invite link
      $('net-code').value = params.get('room').toUpperCase();
      setMode('online');
    }
    if (params.get('auto')) autoTest();
  },
};

// ------------------------------------------------------------------ online
// Each browser simulates its own car (the host also runs the AI cars) and sends its state ~20 times a second
// through the relay in tools/devserver.py; everyone else draws it interpolated (src/netsync.*).
const Net = {
  ws: null, id: 0, room: '', host: 0, players: [], profiles: {}, settings: null,
  race: null,      // { id, opts, lineup, mine: [racer indices simulated here], ready: Set, started, go }
  ptr: 0, floats: 0, sendTimer: 0,

  url() { return params.get('server') || `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`; },
  connected() { return !!(this.ws && this.ws.readyState === 1 && this.id); },
  isHost() { return this.connected() && this.id === this.host; },
  send(msg) { if (this.ws && this.ws.readyState === 1) this.ws.send(JSON.stringify(msg)); },
  name() { return $('net-name').value.trim() || 'Player'; },

  open(first) {
    this.close();
    try { localStorage.setItem('bsr-name', $('net-name').value.trim()); } catch (e) {}
    setStatus('Connecting…');
    const ws = this.ws = new WebSocket(this.url());
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => this.send(first);
    ws.onmessage = (e) => { if (ws === this.ws) this.onMessage(e.data); };
    ws.onclose = () => { if (ws === this.ws) this.onClosed(); };
  },
  close() {
    const ws = this.ws;
    this.ws = null;
    if (ws) ws.close();
    this.onClosed(true);
  },
  onClosed(quiet) {
    const wasConnected = !!this.id;
    this.ws = null;
    this.id = this.host = 0;
    this.room = '';
    this.players = [];
    this.profiles = {};
    this.settings = null;
    if (this.race && race && race.opts.online) {  // finish the race alone: drop everyone else's car
      for (let i = 0; i < this.race.lineup.length; i++)
        if (!this.race.mine.includes(i)) Module.ccall('bsr_remove_racer', null, ['number'], [i]);
      if (!this.race.go) Module.ccall('bsr_set_waiting', null, ['number'], [0]);
    }
    this.endRace();
    if (!quiet) setStatus(wasConnected ? 'Disconnected from the server.' : 'Could not reach the multiplayer server.');
    if (manifest) { refreshSel(); this.renderLobby(); }
  },

  onMessage(data) {
    if (typeof data !== 'string') return this.onStates(data);
    let m;
    try { m = JSON.parse(data); } catch (e) { return; }
    const fromHost = m.from !== undefined && m.from === this.host;
    switch (m.t) {
      case 'welcome':
        this.id = m.id;
        this.room = m.room;
        setStatus('');
        this.sendProfile();
        break;
      case 'peers': {
        const before = new Set(this.players.map((p) => p.id));
        const hostBefore = this.host;
        this.host = m.host;
        this.players = m.players;
        const present = new Set(m.players.map((p) => p.id));
        for (const id of Object.keys(this.profiles)) if (!present.has(+id)) delete this.profiles[id];
        if (this.race) {
          this.race.lineup.forEach((e, i) => {
            // Cars of players who left, and the AI cars if the host (who simulated them) left.
            if ((e.peer && !present.has(e.peer)) || (!e.peer && hostBefore && hostBefore !== this.host))
              Module.ccall('bsr_remove_racer', null, ['number'], [i]);
          });
          this.maybeGo();
        }
        if (this.isHost()) {
          this.settings = null;
          this.pushSettings();
          if (hostBefore !== this.host && this.race && !this.race.go) this.go(true);  // took over mid-countdown
        }
        if ([...present].some((id) => !before.has(id))) this.sendProfile();  // introduce ourselves to newcomers
        refreshSel();
        break;
      }
      case 'error':
        setStatus(m.msg);
        break;
      case 'profile':
        this.profiles[m.from] = { skin: m.skin | 0, cls: manifest.classes[m.cls] ? m.cls : null };
        break;
      case 'settings':
        if (fromHost) this.applySettings(m);
        break;
      case 'race':
        if (fromHost) this.joinRace(m);
        break;
      case 'ready':
        if (this.isHost() && this.race && m.race === this.race.id) {
          this.race.ready.add(m.from);
          if (this.race.started) this.send({ t: 'go', race: this.race.id, to: m.from });  // late loader
          else this.maybeGo();
        }
        break;
      case 'go':
        if (this.race && m.race === this.race.id) this.go();
        break;
      case 'quit':
        if (this.race && m.race === this.race.id)
          this.race.lineup.forEach((e, i) => { if (e.peer === m.from) Module.ccall('bsr_remove_racer', null, ['number'], [i]); });
        break;
    }
    this.renderLobby();
  },

  sendProfile() { if (this.connected()) this.send({ t: 'profile', skin: sel.skin, cls: sel.cls }); },

  pushSettings() {
    if (!this.isHost()) return;
    const s = { track: sel.track, cls: sel.cls, weather: $('weather').value, opponents: +$('opp').value, laps: +$('laps').value,
      minutes: +$('minutes').value, lenMode: sel.lenMode, raceType: sel.raceType };
    const key = JSON.stringify(s);
    if (key === this.settings) return;
    this.settings = key;
    this.send({ t: 'settings', ...s });
  },
  applySettings(m) {
    if (manifest.tracks[m.track]) sel.track = m.track;
    sel.raceType = m.raceType === 'open' ? 'open' : 'cup';
    sel.lenMode = m.lenMode === 'time' ? 'time' : 'laps';
    // Cup races use the host's class; in open races everyone keeps their own.
    if (sel.raceType === 'cup' && manifest.classes[m.cls] && sel.cls !== m.cls) { sel.cls = m.cls; sel.skin = 0; this.sendProfile(); }
    $('cls').value = sel.cls;
    if (RACE_MINUTES.includes(m.minutes)) $('minutes').value = m.minutes;
    if (WEATHER_CATS[m.weather]) $('weather').value = m.weather;
    $('opp').value = m.opponents;
    $('laps').value = m.laps;
    $('laps-v').textContent = m.laps;
    refreshSel();
  },

  renderLobby() {
    const on = this.connected();
    $('online-join').classList.toggle('hidden', on);
    $('online-room').classList.toggle('hidden', !on);
    if (!on) return;
    $('net-room').textContent = this.room;
    const esc = (t) => String(t).replace(/[&<>"]/g, (c) => `&#${c.charCodeAt(0)};`);
    $('net-players').innerHTML = this.players.map((p) => {
      const tags = [p.id === this.host && 'host', p.id === this.id && 'you'].filter(Boolean).join(', ');
      const prof = p.id === this.id ? { skin: sel.skin, cls: sel.cls } : this.profiles[p.id];
      const skin = prof ? prof.skin : null;
      const cls = sel.raceType === 'open' && prof && prof.cls ? ` <span class="tag">${esc(className(prof.cls))}</span>` : '';
      return `<li>${esc(p.name)}${cls}${skin !== null ? ` <span class="tag">${esc(TEAMS[skin] || 'skin ' + (skin + 1))}</span>` : ''}` +
        `${tags ? ` <span class="tag">(${tags})</span>` : ''}</li>`;
    }).join('');
  },

  // Host: pick the grid and tell everyone. Players start at the back in join order, AI cars fill the front.
  startRace() {
    if (!this.isHost() || !manifest) return;
    const open = sel.raceType === 'open';
    const lineup = this.players.slice(0, MAX_RACERS).map((p) => {
      const prof = p.id === this.id ? { skin: sel.skin, cls: sel.cls } : this.profiles[p.id] || { skin: 0 };
      const cls = open && prof.cls ? prof.cls : sel.cls;
      return { peer: p.id, name: p.name, cls, skin: prof.skin % manifest.classes[cls].skins.length };
    });
    const nSkins = manifest.classes[sel.cls].skins.length;
    const used = new Set(lineup.filter((e) => e.cls === sel.cls).map((e) => e.skin));
    const free = [...Array(nSkins).keys()].filter((i) => !used.has(i));
    if (!free.length) free.push(...Array(nSkins).keys());
    const ai = Math.max(0, Math.min(+$('opp').value, MAX_RACERS - lineup.length));
    for (let i = 0; i < ai; i++) {
      const cls = open ? randomClass(sel.cls) : sel.cls;
      const skin = cls === sel.cls ? free[i % free.length] : Math.floor(Math.random() * manifest.classes[cls].skins.length);
      lineup.push({ peer: 0, name: 'CPU', cls, skin });
    }
    const msg = {
      t: 'race', race: 1 + Math.floor(Math.random() * 0xfffffe),  // exact as a float32: tags state packets
      track: sel.track, cls: sel.cls, lineup, song: pickSong(), ...raceLength(),
      ...(({ cat, preset }) => ({ weather: preset, weatherCat: cat }))(pickWeather(sel.track, $('weather').value)),
    };
    this.send(msg);
    this.joinRace({ ...msg, from: this.id });
  },

  async joinRace(m) {
    this.endRace();
    if (race) stopRace();
    const roles = m.lineup.map((e) => (e.peer === this.id ? '0' : !e.peer && this.isHost() ? 'a' : 'r'));
    const opts = {
      online: true, raceId: m.race, track: m.track, cls: m.cls, weather: m.weather, weatherCat: m.weatherCat || '-',
      laps: m.laps, minutes: m.minutes | 0,
      song: m.song,
      racers: m.lineup.map((e, i) => ({ role: roles[i], cls: manifest.classes[e.cls] ? e.cls : m.cls, skin: e.skin,
        name: e.peer === this.id ? 'You' : e.name })),
    };
    const r = this.race = {
      id: m.race, opts, lineup: m.lineup, ready: new Set(), started: false, go: false,
      mine: roles.map((x, i) => (x === 'r' ? -1 : i)).filter((i) => i >= 0),
    };
    const ok = await startRace(opts);
    if (this.race !== r) return;
    if (!ok) { this.send({ t: 'quit', race: r.id }); this.endRace(); show('menu'); return; }
    r.mine = r.mine.filter((i) => i < ok);
    if (r.go) Module.ccall('bsr_set_waiting', null, ['number'], [0]);
    if (!this.ptr) {
      this.floats = Module.ccall('bsr_snapshot_floats', 'number', [], []);
      this.ptr = Module._malloc(this.floats * 4);  // reused for every packet
    }
    clearInterval(this.sendTimer);
    this.sendTimer = setInterval(() => this.sendStates(), 50);
    if (this.isHost()) {
      r.ready.add(this.id);
      r.goTimer = setTimeout(() => this.go(true), 45000);  // don't let one slow loader hold everyone forever
      this.maybeGo();
    } else {
      this.send({ t: 'ready', race: r.id });
    }
  },
  stillCurrent(opts) { return this.race && this.race.opts === opts; },

  maybeGo() {
    const r = this.race;
    if (!this.isHost() || !r || r.started) return;
    const present = new Set(this.players.map((p) => p.id));
    if (r.lineup.every((e) => !e.peer || !present.has(e.peer) || r.ready.has(e.peer))) this.go(true);
  },
  go(broadcast) {
    const r = this.race;
    if (!r) return;
    if (broadcast && this.isHost() && !r.started) {
      r.started = true;
      clearTimeout(r.goTimer);
      this.send({ t: 'go', race: r.id });
    }
    r.go = true;
    if (race && race.opts === r.opts) {
      Module.ccall('bsr_set_waiting', null, ['number'], [0]);
      Announcer.say('racestart_in_3_seconds');
    }
  },

  // Packet: float32 [raceId, count, (racerIndex, snapshot...) * count]
  sendStates() {
    const r = this.race;
    if (!r || !race || race.opts !== r.opts || !this.ws || this.ws.readyState !== 1) return;
    if (this.ws.bufferedAmount > 1 << 15) return;  // link can't keep up: skip rather than queue stale states
    const out = new Float32Array(2 + r.mine.length * (1 + this.floats));
    out[0] = r.id;
    let n = 0;
    for (const i of r.mine) {
      if (!Module.ccall('bsr_get_state', 'number', ['number', 'number'], [i, this.ptr])) continue;
      const o = 2 + n * (1 + this.floats);
      out[o] = i;
      out.set(Module.HEAPF32.subarray(this.ptr >> 2, (this.ptr >> 2) + this.floats), o + 1);
      n++;
    }
    out[1] = n;
    if (n) this.ws.send(out.subarray(0, 2 + n * (1 + this.floats)));
  },
  onStates(buf) {
    const r = this.race;
    if (!r || !race || race.opts !== r.opts || !this.ptr || buf.byteLength % 4) return;
    const f = new Float32Array(buf);
    if (f[0] !== r.id) return;
    const n = Math.min(f[1] | 0, Math.floor((f.length - 2) / (1 + this.floats)));
    for (let k = 0; k < n; k++) {
      const o = 2 + k * (1 + this.floats);
      Module.HEAPF32.set(f.subarray(o + 1, o + 1 + this.floats), this.ptr >> 2);
      Module.ccall('bsr_put_state', null, ['number', 'number'], [f[o] | 0, this.ptr]);
    }
  },

  // Leave the current race (to the lobby); the room stays.
  leaveRace() {
    if (this.race) this.send({ t: 'quit', race: this.race.id });
    this.endRace();
    stopRace();
  },
  endRace() {
    if (this.race) clearTimeout(this.race.goTimer);
    clearInterval(this.sendTimer);
    this.race = null;
  },
};

// ------------------------------------------------------------------ UI wiring
const inMenu = () => !$('menu').classList.contains('hidden');
$('go').onclick = () => {
  if (sel.mode === 'online') return Net.startRace();
  startRace({ track: sel.track, cls: sel.cls, skin: sel.skin, skin2: sel.skin2, split: sel.mode === 'split',
    practice: sel.mode === 'practice', ghost: sel.ghost, raceType: sel.raceType, opponents: +$('opp').value,
    weather: $('weather').value, ...raceLength() });
};
const replay = () => {
  if (lastRace && lastRace.online) Net.startRace();
  else if (lastRace) startRace(lastRace);
};
const online = () => !!(race && race.opts.online);
$('resume').onclick = () => { Module.ccall('bsr_set_paused', null, ['number'], [0]); show(null); $('canvas').focus(); };
$('restart').onclick = replay;
$('again').onclick = replay;
$('quit').onclick = $('tomenu').onclick = () => {
  if (online()) Net.leaveRace();
  else stopRace();
  show('menu');
  refreshSel();
};
$('net-name').value = (() => { try { return localStorage.getItem('bsr-name') || ''; } catch (e) { return ''; } })();
$('net-create').onclick = () => Net.open({ t: 'create', name: Net.name() });
$('net-join').onclick = () => {
  const room = $('net-code').value.trim().toUpperCase();
  if (room.length !== 4) return setStatus('Enter the 4-letter room code.');
  Net.open({ t: 'join', room, name: Net.name() });
};
$('net-code').onkeydown = (e) => { if (e.key === 'Enter') $('net-join').onclick(); };
$('net-leave').onclick = () => { Net.close(); setStatus(''); };
$('net-invite').onclick = () => {
  const url = `${location.origin}${location.pathname}?room=${Net.room}`;
  (navigator.clipboard ? navigator.clipboard.writeText(url) : Promise.reject()).then(
    () => setStatus('Invite link copied.'), () => setStatus(url));
};
window.addEventListener('keydown', (e) => {
  if (e.code === 'Escape' && !inMenu() && $('results').classList.contains('hidden')) {
    const paused = !$('pause').classList.contains('hidden');
    if (!online()) Module.ccall('bsr_set_paused', null, ['number'], [paused ? 0 : 1]);
    show(paused ? null : 'pause');
  }
});
document.addEventListener('visibilitychange', () => {
  if (document.hidden && !online() && !inMenu() && $('pause').classList.contains('hidden') &&
      $('results').classList.contains('hidden')) {
    Module.ccall('bsr_set_paused', null, ['number'], [1]);
    show('pause');
  }
});

// Browsers only start audio after a user gesture: unlock it on the first one and start the menu theme.
for (const ev of ['pointerdown', 'keydown']) {
  window.addEventListener(ev, function unlock() {
    window.removeEventListener('pointerdown', unlock);
    window.removeEventListener('keydown', unlock);
    Audio.init();
    if (Audio.ctx && Audio.ctx.state === 'suspended') Audio.ctx.resume();
    if (inMenu()) Audio.menuMusic();
  });
}

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
  Settings.musicMode = 'off';
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
