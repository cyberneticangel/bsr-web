// Drives a real headless Firefox over WebDriver BiDi (no extra packages): opens the page, runs scripted steps
// and saves screenshots. Used to check the menus, HUD and the WebGL view in an actual browser.
//
// usage: node tools/browsershot.js <url> <out dir> <steps.json | inline JSON>
//   steps: [{ "wait": ms } | { "eval": "js expression" } | { "until": "js expression", "timeout": ms }
//           | { "shot": "name" } | { "key": "KeyW", "down": true }]
// env: FIREFOX (binary), WIDTH, HEIGHT
'use strict';
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const [url, outDir, stepsArg] = process.argv.slice(2);
if (!url || !outDir || !stepsArg) {
  console.error('usage: node tools/browsershot.js <url> <out dir> <steps.json | JSON>');
  process.exit(2);
}
const steps = JSON.parse(fs.existsSync(stepsArg) ? fs.readFileSync(stepsArg, 'utf8') : stepsArg);
const W = +(process.env.WIDTH || 1280), H = +(process.env.HEIGHT || 800);
const port = 9300 + Math.floor(Math.random() * 500);
const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'bsr-ff-'));
fs.writeFileSync(path.join(profile, 'user.js'), [
  'user_pref("webgl.force-enabled", true);',
  'user_pref("media.autoplay.default", 0);',
  'user_pref("browser.shell.checkDefaultBrowser", false);',
].join('\n'));

const ff = spawn(process.env.FIREFOX || 'firefox',
  ['--headless', '--no-remote', '--profile', profile, `--remote-debugging-port=${port}`, `--width=${W}`, `--height=${H}`],
  { stdio: ['ignore', 'ignore', 'pipe'] });
let ffErr = '';
ff.stderr.on('data', (d) => { ffErr += d; });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function connect() {
  for (let i = 0; i < 100; i++) {
    try {
      const ws = new WebSocket(`ws://127.0.0.1:${port}/session`);
      await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
      return ws;
    } catch (e) { await sleep(200); }
  }
  throw new Error('could not connect to Firefox: ' + ffErr.slice(-400));
}

(async () => {
  const ws = await connect();
  let id = 0;
  const pending = new Map();
  ws.onmessage = (e) => {
    const m = JSON.parse(e.data);
    if (m.type === 'event' && m.method === 'log.entryAdded') console.log('[page]', m.params.level, m.params.text);
    if (m.id && pending.has(m.id)) {
      const { res, rej, method } = pending.get(m.id);
      pending.delete(m.id);
      m.type === 'error' ? rej(new Error(`${method} ${m.error}: ${m.message}`)) : res(m.result);
    }
  };
  const cmd = (method, params = {}) => new Promise((res, rej) => {
    const i = ++id;
    pending.set(i, { res, rej, method });
    ws.send(JSON.stringify({ id: i, method, params }));
  });
  await cmd('session.new', { capabilities: {} });
  await cmd('session.subscribe', { events: ['log.entryAdded'] });
  const { contexts } = await cmd('browsingContext.getTree', {});
  const context = contexts[0].context;
  await cmd('browsingContext.navigate', { context, url, wait: 'complete' });
  const evaluate = async (expr) => {
    const r = await cmd('script.evaluate', { expression: expr, target: { context }, awaitPromise: true });
    if (r.type === 'exception') throw new Error('page exception: ' + JSON.stringify(r.exceptionDetails.text));
    return r.result && 'value' in r.result ? r.result.value : undefined;
  };
  fs.mkdirSync(outDir, { recursive: true });
  for (const s of steps) {
    if (s.wait) await sleep(s.wait);
    if (s.eval) console.log('eval', s.eval.slice(0, 60), '->', JSON.stringify(await evaluate(s.eval)));
    if (s.until) {
      const t0 = Date.now();
      while (!(await evaluate(s.until))) {
        if (Date.now() - t0 > (s.timeout || 30000)) throw new Error('timeout waiting for ' + s.until);
        await sleep(200);
      }
    }
    if (s.key) {
      await cmd('input.performActions', { context, actions: [{ type: 'key', id: 'kb',
        actions: [{ type: s.down === false ? 'keyUp' : 'keyDown', value: keyValue(s.key) }] }] });
    }
    if (s.shot) {
      const { data } = await cmd('browsingContext.captureScreenshot', { context });
      const file = path.join(outDir, s.shot + '.png');
      fs.writeFileSync(file, Buffer.from(data, 'base64'));
      console.log('shot', file);
    }
  }
  ws.close();
  ff.kill();
  fs.rmSync(profile, { recursive: true, force: true });
})().catch((e) => {
  console.error('FAIL:', e.message);
  ff.kill();
  process.exit(1);
});

// WebDriver key values for the codes the game listens to.
function keyValue(code) {
  const special = { ArrowUp: '', ArrowDown: '', ArrowLeft: '', ArrowRight: '', Space: ' ',
    Escape: '', Tab: '', Enter: '', Backspace: '', ShiftRight: '' };
  if (special[code]) return special[code];
  if (/^Key[A-Z]$/.test(code)) return code.slice(3).toLowerCase();
  return code;
}
