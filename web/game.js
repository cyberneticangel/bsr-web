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
const MAX_RACERS = 8;  // grid slots on every track

let manifest = null;
const loaded = new Set();
const sel = { mode: 'single', track: 'baanbreker', cls: 'pro_std', skin: 0, skin2: 1 };
let lastRace = null;
let race = null;  // current race: { opts, count, local, names, finished }
const hudStates = [{}, {}];
let hudState = hudStates[0];

function xorInPlace(u8, start = 0) {
  for (let i = start; i < u8.length; i++) u8[i] ^= KEY[(i - start) & 15];
  return u8;
}

// ------------------------------------------------------------------ audio
const Audio = {
  ctx: null, buffers: {}, cars: [], crowd: null, master: null, music: null, musicEl: null,
  lastImpact: [0, 0], enabled: true,
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
  cls.onchange = () => { sel.cls = cls.value; sel.skin = 0; sel.skin2 = 1; refreshSel(); };
  for (const id of ['opp', 'laps']) {
    $(id).oninput = () => { $(id + '-v').textContent = $(id).value; Net.pushSettings(); };
  }
  wx.onchange = () => Net.pushSettings();
  for (const b of $('modes').children) b.onclick = () => setMode(b.dataset.v);
  refreshSel();
}

function skinButtons(container, current, onPick) {
  container.innerHTML = '';
  manifest.classes[sel.cls].skins.forEach((s, i) => {
    const b = document.createElement('button');
    b.textContent = i + 1;
    b.title = s;
    b.classList.toggle('sel', i === current);
    b.onclick = () => { onPick(i); refreshSel(); };
    container.appendChild(b);
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
  const split = sel.mode === 'split', online = sel.mode === 'online';
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
  for (const id of ['cls', 'weather', 'opp', 'laps']) $(id).disabled = guest;
  const go = $('go');
  if (!online) { go.textContent = 'RACE'; go.disabled = false; }
  else if (!Net.connected()) { go.textContent = 'JOIN OR CREATE A ROOM'; go.disabled = true; }
  else if (guest) { go.textContent = 'WAITING FOR HOST'; go.disabled = true; }
  else { go.textContent = 'START RACE'; go.disabled = false; }
  Net.pushSettings();
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
  if (id === 'pause') {  // online the race can't be paused or restarted, only left
    const net = !!race && !!race.opts.online;
    $('pause-title').textContent = net ? 'RACE MENU' : 'PAUSED';
    $('restart').classList.toggle('hidden', net);
    $('quit').textContent = net ? 'LEAVE RACE' : 'QUIT TO MENU';
  }
}

// Grid for a race on this machine: local players first (they start at the back), then AI cars.
function localLineup(opts) {
  const nSkins = manifest.classes[opts.cls].skins.length;
  const humans = opts.split ? [opts.skin, opts.skin2 ?? (opts.skin + 1) % nSkins] : [opts.skin];
  const free = [...Array(nSkins).keys()].filter((i) => !humans.includes(i));
  if (!free.length) free.push(...Array(nSkins).keys());
  const racers = humans.map((skin, p) => ({ role: String(p), skin, name: opts.split ? 'P' + (p + 1) : 'You' }));
  const ai = Math.max(0, Math.min(opts.opponents, MAX_RACERS - humans.length));
  for (let i = 0; i < ai; i++) racers.push({ role: 'a', skin: free[i % free.length], name: 'CPU' });
  return racers;
}

// opts: { track, cls, laps, weather, music?, online?, racers: [{ role, skin, name }] }
//       or the single/split form { track, cls, skin, skin2?, split?, opponents, laps, weather }.
async function startRace(opts) {
  if (!opts.racers) opts = { ...opts, racers: localLineup(opts) };
  Audio.init();
  if (Audio.ctx && Audio.ctx.state === 'suspended') Audio.ctx.resume();
  $('go').disabled = true;
  let ok = 0;
  try {
    const music = opts.music || manifest.music[2 + Math.floor(Math.random() * (manifest.music.length - 2))];
    const files = [...manifest.common, ...manifest.tracks[opts.track], ...Object.values(manifest.sounds), music];
    setStatus('Loading…', 0);
    await fetchFiles(files, (d, n) => setStatus(`Loading ${manifest.trackNames[opts.track]}… ${d}/${n}`, d / n));
    setStatus('Building track…');
    await new Promise((r) => setTimeout(r, 20));
    if (opts.online && !Net.stillCurrent(opts)) return 0;  // host started another race while we loaded
    const skins = manifest.classes[opts.cls].skins;
    const top = String(manifest.classes[opts.cls].topSpeed || 50);
    Module.ccall('bsr_set_online', null, ['number'], [opts.online ? 1 : 0]);
    ok = Module.ccall('bsr_start', 'number', ['string', 'string', 'string', 'number', 'number', 'string', 'string', 'string'],
      [opts.track, opts.cls, opts.racers.map((r) => skins[r.skin % skins.length]).join(','), 0, opts.laps, top,
        opts.weather || 'sunny_bluesky_summer', opts.racers.map((r) => r.role).join('')]);
    if (!ok) throw new Error('engine failed to load track');
    if (opts.online) Module.ccall('bsr_set_waiting', null, ['number'], [1]);  // until every player has loaded
    const local = opts.racers.slice(0, ok).filter((r) => /^[0-9]$/.test(r.role)).length;
    race = { opts, count: ok, local: Math.max(1, local), names: opts.racers.map((r) => r.name), finished: new Set() };
    for (const h of hudStates) for (const k of Object.keys(h)) delete h[k];
    lastRace = opts;
    setStatus('');
    show(null);
    for (const pre of ['h-', 'h2-']) $(pre + 'msg').textContent = '';
    $('h-keys').textContent = local > 1 ? 'WASD drive · Space handbrake · C camera · R reset · P pause'
      : '↑↓←→ drive · Space handbrake · C camera · R reset' + (opts.online ? ' · Esc menu' : ' · P pause');
    $('canvas').focus();
    Audio.startRace(opts.cls, race.local);
    Audio.playMusic(music);
    if (Audio.ctx && !opts.online) Audio.play('FX_Comment_racestart_in_3_seconds_A_MW', 0.9);
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
  const rows = res.map((r) => {
    const name = race.names[r.i] ?? (r.player ? 'You' : 'CPU');
    const time = r.finished ? fmtTime(r.time) : r.gone ? 'left' : 'running';
    return `<tr class="${r.player ? 'me' : ''}"><td>${r.place}</td><td>${esc(name)}</td><td>${time}</td><td>${fmtTime(r.best)}</td></tr>`;
  }).join('');
  $('res-table').innerHTML = '<tr><th>#</th><th>Driver</th><th>Time</th><th>Best lap</th></tr>' + rows;
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
    $(pre + 'time').textContent = fmtTime(h.time);
    $(pre + 'laptime').textContent = fmtTime(h.lapTime);
    $(pre + 'best').textContent = fmtTime(h.best);
    $(pre + 'speed').textContent = Math.round(h.speed);
    $(pre + 'countdown').textContent = h.countdown > 0 && !h.waiting ? h.countdown : '';
    if (h.waiting) $(pre + 'msg').textContent = 'Waiting for players…';
    else if ($(pre + 'msg').textContent === 'Waiting for players…') $(pre + 'msg').textContent = '';
  },
  onAudio(rpm, thr, skid, impact, speed, p = 0) { Audio.update(rpm, thr, skid, impact, speed, p); },
  onEvent(event) {
    const [name, ps] = event.split(':');
    const p = ps === undefined ? 0 : +ps;
    const h = hudStates[p];
    const who = race && race.local > 1 ? `P${p + 1} ` : '';
    if (name === 'beep') Audio.play('menu_updown', 0.8);
    else if (name === 'go') { Audio.play('menu_action', 1); flash('GO!', 900); }
    else if (name === 'lap') flash('LAP ' + h.lap, 1500, p);
    else if (name === 'lastlap') flash('FINAL LAP', 1500, p);
    else if (name === 'finish') {
      Audio.play('FX_horn_finish_22_mo_A', 0.9);
      const first = h.place === 1;
      flash(first ? (who ? who + 'WINS!' : 'YOU WIN!') : who + 'FINISHED P' + h.place, 3000, p);
      if (!race) return;
      race.finished.add(p);
      if (race.finished.size < race.local) return;  // results once every local player is home
      const r = race;
      setTimeout(() => Audio.play(first ? 'FX_Comment_finish_number_1_A_MW' : 'FX_Comment_race_over_D_MW', 0.9), 900);
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
        this.profiles[m.from] = { skin: m.skin | 0 };
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

  sendProfile() { if (this.connected()) this.send({ t: 'profile', skin: sel.skin }); },

  pushSettings() {
    if (!this.isHost()) return;
    const s = { track: sel.track, cls: sel.cls, weather: $('weather').value, opponents: +$('opp').value, laps: +$('laps').value };
    const key = JSON.stringify(s);
    if (key === this.settings) return;
    this.settings = key;
    this.send({ t: 'settings', ...s });
  },
  applySettings(m) {
    if (manifest.tracks[m.track]) sel.track = m.track;
    if (manifest.classes[m.cls] && sel.cls !== m.cls) { sel.cls = m.cls; sel.skin = 0; this.sendProfile(); }
    $('cls').value = sel.cls;
    $('weather').value = m.weather;
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
      const skin = p.id === this.id ? sel.skin : this.profiles[p.id] ? this.profiles[p.id].skin : null;
      return `<li>${esc(p.name)}${skin !== null ? ` <span class="tag">skin ${skin + 1}</span>` : ''}` +
        `${tags ? ` <span class="tag">(${tags})</span>` : ''}</li>`;
    }).join('');
  },

  // Host: pick the grid and tell everyone. Players start at the back in join order, AI cars fill the front.
  startRace() {
    if (!this.isHost() || !manifest) return;
    const nSkins = manifest.classes[sel.cls].skins.length;
    const lineup = this.players.slice(0, MAX_RACERS).map((p) => ({
      peer: p.id, name: p.name, skin: (p.id === this.id ? sel.skin : (this.profiles[p.id] || { skin: 0 }).skin) % nSkins,
    }));
    const used = new Set(lineup.map((e) => e.skin));
    const free = [...Array(nSkins).keys()].filter((i) => !used.has(i));
    if (!free.length) free.push(...Array(nSkins).keys());
    const ai = Math.max(0, Math.min(+$('opp').value, MAX_RACERS - lineup.length));
    for (let i = 0; i < ai; i++) lineup.push({ peer: 0, name: 'CPU', skin: free[i % free.length] });
    const msg = {
      t: 'race', race: 1 + Math.floor(Math.random() * 0xfffffe),  // exact as a float32: tags state packets
      track: sel.track, cls: sel.cls, weather: $('weather').value, laps: +$('laps').value, lineup,
      music: manifest.music[2 + Math.floor(Math.random() * (manifest.music.length - 2))],
    };
    this.send(msg);
    this.joinRace({ ...msg, from: this.id });
  },

  async joinRace(m) {
    this.endRace();
    if (race) stopRace();
    const roles = m.lineup.map((e) => (e.peer === this.id ? '0' : !e.peer && this.isHost() ? 'a' : 'r'));
    const opts = {
      online: true, raceId: m.race, track: m.track, cls: m.cls, weather: m.weather, laps: m.laps, music: m.music,
      racers: m.lineup.map((e, i) => ({ role: roles[i], skin: e.skin, name: e.peer === this.id ? 'You' : e.name })),
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
      if (Audio.ctx) Audio.play('FX_Comment_racestart_in_3_seconds_A_MW', 0.9);
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
    opponents: +$('opp').value, laps: +$('laps').value, weather: $('weather').value });
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
