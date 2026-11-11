// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Real CPython -> native Python Mario. CDP is only the external test driver;
// the sample and fixtures have no JavaScript runtime dependency.
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';

const option = (name, fallback) => process.argv.slice(2)
    .find(arg => arg.startsWith(`--${name}=`))?.slice(name.length + 3) ?? fallback;
const binary = path.resolve(option('chrome-binary', 'out/Debug/chrome.exe'));
const output = path.resolve(option('output', 'out/Debug/pyscript-mario-smoke.json'));
const chromeArgs = process.argv.slice(2).filter(arg => arg.startsWith('--chrome-arg='))
    .map(arg => arg.slice('--chrome-arg='.length));
assert.ok(!chromeArgs.some(arg => /no-sandbox|disable-web-security/.test(arg)));
fs.mkdirSync(path.dirname(output), {recursive: true});
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'pyscript-mario-'));
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));

const sample = fs.readFileSync('chrome/test/data/pyscript/mario.html', 'utf8');
const reference = JSON.parse(fs.readFileSync('chrome/test/data/pyscript/mario/reference.json'));
const literal = value => value === null ? 'None' : typeof value === 'boolean' ? (value ? 'True' : 'False') :
    Array.isArray(value) ? '['+value.map(literal).join(',')+']' : typeof value === 'object' ?
    '{'+Object.entries(value).map(([k,v]) => literal(k)+':'+literal(v)).join(',')+'}' : JSON.stringify(value);
const boot = sample.indexOf('<!-- boot.py -->');
assert.ok(boot > 0);
function fixture(scenario) {
  const expected = scenario.expected;
  const program = `
startGame(${scenario.seed ?? 0x4D415249})
vars(player).update(${literal(scenario.player ?? {})})
vars(game).update(${literal(scenario.game ?? {})})
checkpoints = ${literal(expected.map(c => c.step))}
actions = ${literal(scenario.actions)}
tick = 0

def capture():
    return dict(game=vars(game).copy(), player=vars(player).copy(),
        usedBlocks=[i for i,b in enumerate(blocks) if b.used],
        collectedCoins=[i for i,c in enumerate(coins) if c.collected],
        enemies=[vars(e).copy() for e in enemies], particles=[vars(p).copy() for p in particles[:2]],
        particleCount=len(particles), floatTexts=[vars(t).copy() for t in floatTexts], rng=rng.state)

def observe(timestamp):
    print('ORACLE', tick, repr(capture()))
    animation.request_frame(finish_build if tick == ${scenario.steps} else replay)

def finish_build(timestamp):
    global finalBatch
    finalBatch = draw()
    animation.request_frame(finish_submit)

def finish_submit(timestamp):
    canvas.submit(screen, finalBatch)
    dom.set_text(statusNode, 'verified:'+str(tick))
    dom.set_text(telemetryNode, game.state+':'+str(game.score)+':'+str(game.checkpoint))

def replay(timestamp):
    global tick
    for _ in range(1):
        if tick >= ${scenario.steps}: break
        for at, key, down in actions:
            if at == tick:
                if key == 'death': loseLife('fixture')
                elif key == 'jump' and down: pressJump()
                else: setattr(input, key, down)
        update(STEP)
        tick += 1
        if tick in checkpoints:
            animation.request_frame(observe)
            return
    animation.request_frame(replay)

canvas.submit(screen, draw())
animation.request_frame(replay)
`;
  // Each fixture retains the same CSP and native budgets as the real sample.
  assert.ok(Buffer.byteLength(program) <= 65536);
  return sample.slice(0, boot)+`<pyscript nonce="python-mario">${program}</pyscript></html>`;
}
const server = http.createServer((request, response) => {
  const scenario = reference.scenarios.find(s => '/oracle/'+s.name === request.url);
  const body = request.url === '/rectangles' ? `<!doctype html><canvas id="game" width="32" height="16"></canvas><p id="status"></p>
<pyscript>
import canvas, dom
c = canvas.get_context_2d(dom.get_element_by_id('game'))
b = canvas.batch()
b.fill_rects([(0, 0, 16, 16, '#f00'), (16, 0, 16, 16, '#0f0')])
try: b.fill_rects([(0, 0, 32, 16, '#00f'), (0, 0, 1, 1, 'red')])
except ValueError: pass
else: raise AssertionError('invalid trailing rectangle')
s = canvas.submit(c, b)
dom.set_text(dom.get_element_by_id('status'), str(s['commands'])+':'+str(s['bytes']))
</pyscript>` : scenario ? fixture(scenario) : request.url === '/sample' ? sample :
      request.url === '/other' ? '<!doctype html><p id="status">other</p>' : undefined;
  if (!body) return response.writeHead(404).end();
  response.writeHead(200, {'Content-Type': 'text/html; charset=utf-8'});
  response.end(body);
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
const report = {binary, origin, sandboxDisabled: false, chromeArgs,
  originalSha256: reference.originalSha256, cases: []};
async function waitFor(predicate, timeout = 30000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (nativeLogs().some(e => e.level === 'error' && /pyscript/.test(e.text))) throw new Error(JSON.stringify(errors()));
    const value = await predicate();
    if (value) return value;
    await delay(50);
  }
  throw new Error(`Condition timed out after ${timeout} ms`);
}
let child, ws, log, port;
const events = [];
const pending = new Map();
let sequence = 0;
async function close() {
  ws?.close();
  if (child?.exitCode === null && child.pid) {
    if (process.platform === 'win32') {
      spawnSync('taskkill', ['/PID', String(child.pid), '/T', '/F'], {windowsHide: true});
    } else {
      child.kill('SIGKILL');
    }
    await Promise.race([new Promise(resolve => child.once('close', resolve)), delay(5000)]);
  }
  if (log !== undefined) { fs.closeSync(log); log = undefined; }
}
async function launch(unrestricted = false) {
  const profile = path.join(root, unrestricted ? 'unrestricted' : 'restricted');
  fs.mkdirSync(profile);
  log = fs.openSync(`${output}.chrome.log`, unrestricted ? 'a' : 'w');
  child = spawn(binary, ['--headless', '--disable-gpu', '--no-first-run',
    '--no-default-browser-check', '--disable-background-networking', '--no-proxy-server',
    '--enable-logging=stderr', '--remote-debugging-port=0', `--user-data-dir=${profile}`,
    '--enable-blink-features=PythonScript', `--native-python-allowed-origin=${origin}`,
    ...(unrestricted ? ['--native-python-unrestricted-pages'] : []),
    ...chromeArgs, 'about:blank'], {windowsHide: true, stdio: ['ignore', log, log]});
  let launchError;
  child.on('error', error => { launchError = error; });
  port = await waitFor(() => {
    if (launchError) throw launchError;
    assert.equal(child.exitCode, null, `Chrome exited: ${child.exitCode}`);
    const file = path.join(profile, 'DevToolsActivePort');
    return fs.existsSync(file) && fs.readFileSync(file, 'utf8').split('\n')[0];
  });
  const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  ws = new WebSocket(targets.find(target => target.type === 'page').webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
  ws.onmessage = event => {
    const message = JSON.parse(event.data);
    if (message.id) {
      pending.get(message.id)?.(message);
      pending.delete(message.id);
    } else {
      events.push(message);
    }
  };
  ws.onclose = () => {
    for (const resolve of pending.values()) resolve({error: {message: 'CDP closed'}});
    pending.clear();
  };
  for (const domain of ['Page', 'Runtime', 'Log']) await send(`${domain}.enable`);
}
function send(method, params = {}) {
  return new Promise((resolve, reject) => {
    const id = ++sequence;
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`CDP timed out: ${method}`));
    }, 20000);
    pending.set(id, message => {
      clearTimeout(timer);
      message.error ? reject(new Error(JSON.stringify(message.error))) : resolve(message.result);
    });
    ws.send(JSON.stringify({id, method, params}));
  });
}
async function evaluate(expression) {
  const value = await send('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true});
  assert.ok(!value.exceptionDetails, JSON.stringify(value.exceptionDetails));
  return value.result.value;
}
const nativeLogs = () => events.filter(event => event.method === 'Log.entryAdded' &&
    event.params.entry.source === 'other').map(event => event.params.entry);
const text = () => evaluate('document.querySelector("#result").textContent');
const click = (id = 'button') => evaluate(`document.getElementById(${JSON.stringify(id)}).click()`);
async function navigate(route) {
  events.length = 0;
  const navigation = await send('Page.navigate', {url: origin + route});
  assert.ok(!navigation.errorText, navigation.errorText);
  await waitFor(() => events.some(event => event.method === 'Page.loadEventFired'));
  assert.equal(await evaluate('location.href'), origin + route);
}
async function check(name, callback) {
  const start = Date.now();
  try {
    const detail = await callback();
    report.cases.push({name, passed: true, elapsedMs: Date.now() - start, detail});
    console.log(`PASS ${name}`);
  } catch (error) {
    report.cases.push({name, passed: false, elapsedMs: Date.now() - start,
      error: String(error.stack ?? error), nativeLogs: nativeLogs(), pageState: await evaluate("({status:document.getElementById('status')?.textContent,telemetry:document.getElementById('telemetry')?.textContent,metrics:document.getElementById('metrics')?.textContent})").catch(() => null)});
    throw error;
  }
}
const errors = () => nativeLogs().filter(entry => entry.level === 'error');
const read = id => evaluate(`document.getElementById(${JSON.stringify(id)}).textContent`);
async function key(type, code, keyValue, virtualKey, extra = {}) {
  await send('Input.dispatchKeyEvent', {type, code, key: keyValue,
    windowsVirtualKeyCode: virtualKey, nativeVirtualKeyCode: virtualKey, ...extra});
}
const pixel = (x, y) => evaluate(`Array.from(document.getElementById('game').getContext('2d').getImageData(${x},${y},1,1).data)`);
async function healthy(route, expected) {
  await navigate(route);
  if (expected) assert.equal(await text(), expected);
  assert.deepEqual(errors(), []);
}
const state = async () => Object.fromEntries((await read('telemetry')).split('|').map(x => x.split(':')));
async function trustedClick(id) {
  await evaluate(`globalThis.__nativeClick=null;document.getElementById(${JSON.stringify(id)}).addEventListener('click',e=>globalThis.__nativeClick={id:e.currentTarget.id,trusted:e.isTrusted},{once:true})`);
  const rect = await evaluate(`(() => {const r=document.getElementById(${JSON.stringify(id)}).getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()`);
  await send('Input.dispatchMouseEvent',{...rect,type:'mouseMoved',buttons:0});
  for (const type of ['mousePressed', 'mouseReleased']) await send('Input.dispatchMouseEvent',
      {...rect, type, button: 'left', buttons:type === 'mousePressed' ? 1 : 0, clickCount: 1});
  assert.deepEqual(await evaluate('globalThis.__nativeClick'),{id,trusted:true});
}
try {
  await launch();
  await send('Emulation.setDeviceMetricsOverride', {width:1120,height:900,deviceScaleFactor:1,mobile:false});
  await check('Grouped rectangles preserve native pixels costs and atomic rejection', async () => {
    await navigate('/rectangles');
    assert.equal(await read('status'), '4:520');
    assert.deepEqual(await pixel(4,4), [255,0,0,255]);
    assert.deepEqual(await pixel(20,4), [0,255,0,255]);
    assert.deepEqual(errors(), []);
  });
  await check('Python-only game initializes under nonce CSP and existing sandbox', async () => {
    await navigate('/sample'); await send('Page.bringToFront');
    await waitFor(async () => (await state()).state === 'ready');
    assert.equal(await evaluate('document.scripts.length'), 0);
    assert.equal(await evaluate('document.querySelectorAll("[onclick],[onkeydown],[onload]").length'), 0);
    assert.deepEqual(errors(), []);
    const screenshot = await send('Page.captureScreenshot', {format: 'png'});
    fs.writeFileSync(`${output}.ready.png`, Buffer.from(screenshot.data, 'base64'));
    return {state: await state(), metrics: await read('metrics')};
  });
  await check('Trusted start movement and variable jump use native Python callbacks', async () => {
    await trustedClick('start');
    // Focus the canvas area so Space is a game key rather than a button click.
    await send('Input.dispatchMouseEvent', {type:'mousePressed',x:500,y:200,button:'left',buttons:1,clickCount:1});
    await send('Input.dispatchMouseEvent', {type:'mouseReleased',x:500,y:200,button:'left',buttons:0,clickCount:1});
    await waitFor(async () => (await state()).state === 'playing');
    await key('keyDown', 'ArrowRight', 'ArrowRight', 39);
    await waitFor(async () => +(await state()).x > 180);
    await key('keyDown', 'Space', ' ', 32);
    await waitFor(async () => +(await state()).y < 400);
    await key('keyUp', 'Space', ' ', 32);
    await key('keyUp', 'ArrowRight', 'ArrowRight', 39);
    await delay(300);
    assert.deepEqual(errors(), []);
    const screenshot = await send('Page.captureScreenshot', {format: 'png'});
    fs.writeFileSync(`${output}.playing.png`, Buffer.from(screenshot.data, 'base64'));
    return {state: await state(), metrics: await read('metrics')};
  });
  await check('Pause clears held controls and stops world time', async () => {
    await key('keyDown', 'KeyP', 'p', 80); await key('keyUp', 'KeyP', 'p', 80);
    await waitFor(async () => (await state()).state === 'paused');
    const before = await state(); await delay(450); const after = await state();
    assert.equal(after.elapsed, before.elapsed); assert.equal(after.keys, '0');
    assert.deepEqual(errors(), []);
    return {before, after};
  });
  await check('Restart is repeatable without allocating more native surfaces', async () => {
    for (let n=0;n<4;n++) { await trustedClick('start'); await delay(280); }
    const s = await state(); assert.equal(s.state,'playing'); assert.equal(s.lives,'3');
    assert.ok(+s.x < 100); assert.equal(s.coins,'0'); assert.deepEqual(errors(), []);
  });
  await check('Pointer capture moves and releasing stops the player', async () => {
    const r = await evaluate('(() => {const r=document.getElementById("right").getBoundingClientRect();return {x:r.x+20,y:r.y+20}})()');
    await send('Input.dispatchMouseEvent', {...r,type:'mousePressed',button:'left',buttons:1,clickCount:1});
    await waitFor(async () => +(await state()).x > 130);
    await send('Input.dispatchMouseEvent', {x:1000,y:100,type:'mouseReleased',button:'left',buttons:0,clickCount:1});
    await waitFor(async () => (await state()).keys === '0' && Math.abs(+(await state()).vx) < 1);
    assert.deepEqual(errors(), []);
  });
  await check('Hidden and frozen page clears input and resets the time baseline', async () => {
    await key('keyDown','ArrowRight','ArrowRight',39);
    await send('Page.setWebLifecycleState',{state:'frozen'}); await delay(300);
    const before=await read('telemetry'); await delay(300); assert.equal(await read('telemetry'),before);
    await send('Page.setWebLifecycleState',{state:'active'});
    const {targetId}=await send('Target.createTarget',{url:'about:blank'});
    await send('Target.activateTarget',{targetId}); await send('Page.bringToFront');
    await send('Target.closeTarget',{targetId});
    await waitFor(async () => (await state()).state === 'paused' && (await state()).keys === '0');
    const paused=await state(); await delay(400); assert.equal((await state()).elapsed,paused.elapsed);
    await key('keyUp','ArrowRight','ArrowRight',39);
    assert.deepEqual(errors(), []);
  });
  for (const scenario of reference.scenarios) {
    await check('Embedded original-JavaScript oracle: '+scenario.name, async () => {
      await navigate('/oracle/'+scenario.name);
      await waitFor(async () => (await read('status')) === 'verified:'+scenario.steps,
          Math.max(30000,scenario.steps*100));
      assert.deepEqual(errors(), []);
      const observed = nativeLogs().filter(e => e.level === 'info' && /ORACLE /.test(e.text))
          .flatMap(e => e.text.split('\n').filter(line => /ORACLE \d+ /.test(line)))
          .map(line => {const m=/ORACLE (\d+) (.*)$/.exec(line); return {step:+m[1],
            state:JSON.parse(m[2].replaceAll("'",'"').replaceAll('True','true').replaceAll('False','false'))};});
      // Only the fixture's closed set of ASCII keys/colors/state names is emitted.
      function compare(a,b,where='') {
        if(typeof a === 'number') assert.ok(Math.abs(a-b)<1e-7, `${where}: ${a} != ${b}`);
        else if(a && typeof a === 'object') {
          assert.deepEqual(Object.keys(a),Object.keys(b),where);
          for(const k of Object.keys(a)) compare(a[k],b[k],where+'.'+k);
        } else assert.equal(a,b,where);
      }
      compare(scenario.expected,observed);
      if (scenario.name === 'full-level-route') {
        const screenshot=await send('Page.captureScreenshot',{format:'png'});
        fs.writeFileSync(`${output}.won.png`,Buffer.from(screenshot.data,'base64'));
      }
      return {steps:scenario.steps, checkpoints:scenario.expected.length, final:await read('telemetry')};
    });
  }
  await check('Navigation and reload discard prior game callbacks and resources', async () => {
    await navigate('/other'); await delay(200); assert.equal(await read('status'),'other');
    await navigate('/sample'); await waitFor(async () => (await state()).state === 'ready');
    events.length=0; await send('Page.reload');
    await waitFor(() => events.some(e => e.method === 'Page.loadEventFired'));
    await waitFor(async () => (await state()).state === 'ready');
    assert.deepEqual(errors(), []);
  });
  await check('Game closes normally with a pending native animation frame', async () => {
    await send('Browser.close'); await waitFor(() => child.exitCode !== null);
    assert.equal(child.exitCode,0);
  });
  report.passed=true;
} catch(error) {
  report.passed=false; report.error=String(error.stack ?? error); process.exitCode=1;
} finally {
  await close(); await new Promise(resolve => server.close(resolve));
  fs.writeFileSync(output,JSON.stringify(report,null,2)+'\n');
  assert.equal(path.dirname(root),path.resolve(os.tmpdir()));
  assert.ok(path.basename(root).startsWith('pyscript-mario-'));
  fs.rmSync(root,{recursive:true,force:true,maxRetries:8,retryDelay:250});
}
console.log(JSON.stringify({passed:report.passed,cases:report.cases.length,output,error:report.error},null,2));
