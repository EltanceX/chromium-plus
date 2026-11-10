// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Real CPython -> native Blink Canvas 2D. CDP is only the external test driver;
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
const output = path.resolve(option('output', 'out/Debug/pyscript-canvas-smoke.json'));
const chromeArgs = process.argv.slice(2).filter(arg => arg.startsWith('--chrome-arg='))
    .map(arg => arg.slice('--chrome-arg='.length));
assert.ok(!chromeArgs.some(arg => /no-sandbox|disable-web-security/.test(arg)));
fs.mkdirSync(path.dirname(output), {recursive: true});
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'pyscript-canvas-'));
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));

const wrap = source => `<!doctype html><meta charset="utf-8">${source}`;
const page = (source, acquire = true) => ({body: wrap(
    '<p id="result">ready</p><button id="button">Run</button>' +
    '<canvas id="game" width="128" height="128"></canvas>' +
    `<pyscript>import canvas, dom, animation
target = dom.get_element_by_id('result')
element = dom.get_element_by_id('game')
${acquire ? 'c = canvas.get_context_2d(element)' : ''}
def expect(error, fn):
    try: fn()
    except error: return
    raise AssertionError('expected ' + error.__name__)
</pyscript><pyscript>${source}</pyscript>`)});
const pages = new Map([
  ['/sample', {file: 'chrome/test/data/pyscript/canvas-scene.html',
    headers: {'Content-Security-Policy': "script-src 'nonce-python-canvas'"}}],
  ['/other', {body: wrap('<p id="result">other</p>')}],
  ['/primitives', page(`
b = canvas.batch()
b.fill_style('#00ff00'); b.fill_rect(0, 0, 128, 128)
b.clear_rect(0, 0, 4, 4)
b.save(); b.translate(10, 10); b.scale(2, 2); b.rotate(0)
b.fill_style('#ff0000'); b.fill_rect(0, 0, 10, 10); b.restore()
b.save(); b.alpha(0.5); b.fill_style('#0000ff'); b.fill_rect(40, 10, 10, 10); b.restore()
b.begin_path(); b.move_to(10, 40); b.line_to(30, 40); b.line_to(20, 60)
b.close_path(); b.fill_style('#ff00ff'); b.fill()
b.begin_path(); b.arc(60, 50, 8, 0, 6.283185, False); b.fill_style('#0000ff'); b.fill()
b.begin_path(); b.ellipse(90, 50, 10, 6, 0, 0, 6.283185, False); b.fill()
b.stroke_style('#000000'); b.line_width(2); b.line_dash([4, 2]); b.stroke_rect(5, 70, 25, 15)
b.line_dash([]); b.begin_path(); b.move_to(40, 75); b.line_to(60, 75); b.stroke()
g = b.linear_gradient(70, 70, 100, 70)
g.add_color_stop(0, '#ff0000'); g.add_color_stop(1, '#0000ff')
b.fill_style(g); b.fill_rect(70, 70, 30, 15)
b.stroke_style(g); b.stroke_rect(70, 90, 30, 4)
b.font(14, 'monospace', True); b.text_align('left'); b.text_baseline('top')
b.fill_style('#000000'); b.fill_text('Python', 5, 104)
stats = canvas.submit(c, b)
dom.set_text(target, str(stats))
`) ],
  ['/transaction', page(`
b = canvas.batch(); b.fill_style('#00ff00'); b.fill_rect(0, 0, 128, 128)
canvas.submit(c, b)
bad = canvas.batch(); bad.fill_style('#ff0000'); bad.fill_rect(0, 0, 128, 128)
bad.alpha(2)
expect(ValueError, lambda: canvas.submit(c, bad))
expect(RuntimeError, lambda: bad.fill_rect(1, 1, 1, 1))
dom.set_text(target, 'rejected')
`) ],
  ['/replacement', page(`
b = canvas.batch(); b.fill_style('#f00'); b.fill_rect(0, 0, 10, 10)
b.alpha(0.1); b.translate(50, 50); canvas.submit(c, b)
next_batch = canvas.batch(); next_batch.fill_rect(20, 20, 10, 10)
canvas.submit(c, next_batch)
canvas.submit(c, next_batch)
expect(RuntimeError, lambda: next_batch.fill())
dom.set_text(target, 'reused')
`) ],
  ['/surface', page(`
s = canvas.create_surface(2, 2)
b = canvas.batch(); b.fill_style('#a020f0'); b.fill_rect(0, 0, 2, 2); canvas.submit(s, b)
b = canvas.batch(); b.smoothing(False); b.draw_surface(s, 0, 0, 128, 128)
stats = canvas.submit(c, b)
bad = canvas.batch(); bad.clear_rect(0, 0, 128, 128); bad.draw_surface(c, 0, 0, 1, 1)
expect(ReferenceError, lambda: canvas.submit(c, bad))
canvas.release(s)
expect(ReferenceError, lambda: canvas.submit(c, b))
expect(ReferenceError, lambda: canvas.release(s))
dom.set_text(target, str(stats))
`) ],
  ['/resize', page(`
old = c
c = canvas.resize(c, 64, 32)
expect(ReferenceError, lambda: canvas.submit(old, canvas.batch()))
expect(PermissionError, lambda: canvas.release(c))
b = canvas.batch(); b.fill_style('#123456'); b.fill_rect(0, 0, 64, 32)
canvas.submit(c, b)
dom.set_text(target, 'resized')
`) ],
  ['/external-resize', page(`
b = canvas.batch(); b.fill_style('#ff0000'); b.fill_rect(0, 0, 20, 20)
canvas.submit(c, b)
def run(e):
    global c
    expect(ReferenceError, lambda: canvas.submit(c, b))
    c = canvas.get_context_2d(element)
    canvas.submit(c, b)
    dom.set_text(target, 'reacquired')
dom.add_event_listener(dom.get_element_by_id('button'), 'click', run)
`) ],
  ['/count', page(`
surfaces = [canvas.create_surface(1, 1) for _ in range(7)]
expect(ValueError, lambda: canvas.create_surface(1, 1))
old = surfaces.pop()
canvas.release(old)
fresh = canvas.create_surface(1, 1)
expect(ReferenceError, lambda: canvas.submit(old, canvas.batch()))
canvas.submit(fresh, canvas.batch())
dom.set_text(target, 'bounded')
`) ],
  ['/pixels', page(`
a = canvas.create_surface(2048, 2048)
b = canvas.create_surface(2048, 2048)
expect(ValueError, lambda: canvas.create_surface(1, 1))
canvas.release(a)
c = canvas.create_surface(1, 1)
expect(ValueError, lambda: canvas.resize(c, 2049, 1))
expect(ValueError, lambda: canvas.create_surface(0, 1))
canvas.release(b); canvas.release(c)
dom.set_text(target, 'pixels-bounded')
`, false)],
  ['/gradients', page(`
b = canvas.batch(); other = canvas.batch()
g = b.linear_gradient(0, 0, 1, 1)
expect(ValueError, lambda: other.fill_style(g))
for _ in range(16): g.add_color_stop(0.5, '#fff')
g.add_color_stop(1, '#000')
expect(ValueError, lambda: canvas.submit(c, b))
dom.set_text(target, 'gradients-bounded')
`) ],
  ['/arguments', page(`
b = canvas.batch()
class Evil:
    def __float__(self): raise AssertionError('arbitrary conversion')
for operation in (lambda: b.fill_rect(float('nan'), 0, 1, 1),
                  lambda: b.fill_rect(float('inf'), 0, 1, 1),
                  lambda: b.fill_text('x' * 1025, 0, 0),
                  lambda: b.fill_style('red')):
    expect(ValueError, operation)
expect(TypeError, lambda: b.fill_rect(Evil(), 0, 1, 1))
expect(TypeError, lambda: b.smoothing(1))
expect(TypeError, lambda: canvas.get_context_2d({}))
for operation in (lambda q: q.font(129), lambda q: q.restore(),
                  lambda q: q.line_dash([-1]), lambda q: q.text_align('invalid')):
    q = canvas.batch(); operation(q)
    expect(ValueError, lambda: canvas.submit(c, q))
dom.set_text(target, 'arguments-bounded')
`) ],
  ['/invalid-node', page(`
b = canvas.batch(); b.fill_style('#00ff00'); b.fill_rect(0, 0, 128, 128)
canvas.submit(c, b)
def run(e):
    expect(ReferenceError, lambda: canvas.submit(c, b))
    dom.set_text(target, 'expired')
dom.add_event_listener(dom.get_element_by_id('button'), 'click', run)
`) ],
  ['/existing-context', page(`
def run(e):
    expect(PermissionError, lambda: canvas.get_context_2d(element))
    dom.set_text(target, 'owned-by-js')
dom.add_event_listener(dom.get_element_by_id('button'), 'click', run)
`, false)],
  ['/stop', page(`
s = canvas.create_surface(2048, 2048)
b = canvas.batch()
for _ in range(8): b.fill_rect(0, 0, 2048, 2048)
dom.set_text(target, 'before')
try: canvas.submit(s, b)
except RuntimeError: pass
dom.set_text(target, 'forbidden')
`) ],
]);
const server = http.createServer((request, response) => {
  const page = pages.get(request.url);
  if (!page) return response.writeHead(404).end();
  response.writeHead(200, {'Content-Type': 'text/html; charset=utf-8', ...page.headers});
  response.end(page.file ? fs.readFileSync(path.resolve(page.file)) : page.body);
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
const imageServer = http.createServer((request, response) => {
  response.writeHead(200, {'Content-Type': 'image/svg+xml'});
  response.end('<svg xmlns="http://www.w3.org/2000/svg" width="2" height="2"><rect width="2" height="2" fill="red"/></svg>');
});
await new Promise(resolve => imageServer.listen(0, '127.0.0.1', resolve));
const imageOrigin = `http://127.0.0.1:${imageServer.address().port}`;
const report = {binary, origin, sandboxDisabled: false, chromeArgs, cases: []};
async function waitFor(predicate, timeout = 30000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
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
      error: String(error.stack ?? error), nativeLogs: nativeLogs()});
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
try {
  await launch();
  await check('Native rectangles transforms alpha and clear produce expected pixels', async () => {
    await healthy('/primitives');
    assert.deepEqual(await pixel(1, 1), [0, 0, 0, 0]);
    assert.deepEqual(await pixel(15, 15), [255, 0, 0, 255]);
    const alpha = await pixel(45, 15);
    assert.ok(alpha[0] === 0 && Math.abs(alpha[1]-127) <= 1 && Math.abs(alpha[2]-128) <= 1 && alpha[3] === 255);
    return {statistics: await text(), alpha};
  });
  await check('Paths arcs ellipses dashes gradients and text use native Canvas', async () => {
    assert.deepEqual(await pixel(20, 45), [255, 0, 255, 255]);
    assert.deepEqual(await pixel(60, 50), [0, 0, 255, 255]);
    assert.deepEqual(await pixel(90, 50), [0, 0, 255, 255]);
    const left = await pixel(72, 75), right = await pixel(97, 75);
    assert.ok(left[0] > left[2] && right[2] > right[0]);
    assert.deepEqual(await pixel(50, 74), [0, 0, 0, 255]);
    const black = await evaluate(`(() => {const d=document.getElementById('game').getContext('2d').getImageData(5,104,70,20).data;let n=0;for(let i=0;i<d.length;i+=4)if(d[i]<100&&d[i+1]<100&&d[i+2]<100&&d[i+3])n++;return n})()`);
    assert.ok(black > 40);
    return {left, right, textPixels: black};
  });
  await check('Invalid trailing command rejects the whole batch before drawing', async () => {
    await healthy('/transaction', 'rejected');
    assert.deepEqual(await pixel(64, 64), [0, 255, 0, 255]);
  });
  await check('Successful submission replaces pixels and resets state and sealed batches can be reused', async () => {
    await healthy('/replacement', 'reused');
    assert.deepEqual(await pixel(5, 5), [0, 0, 0, 0]);
    assert.deepEqual(await pixel(25, 25), [0, 0, 0, 255]);
    assert.deepEqual(await pixel(75, 75), [0, 0, 0, 0]);
  });
  await check('Cache copy smoothing self-copy and released sources obey preflight', async () => {
    await healthy('/surface');
    assert.deepEqual(await pixel(64, 64), [160, 32, 240, 255]);
    return {statistics: await text()};
  });
  await check('Native resize changes dimensions and expires old context versions', async () => {
    await healthy('/resize', 'resized');
    assert.deepEqual(await evaluate("[game.width,game.height]"), [64, 32]);
    assert.deepEqual(await pixel(32, 16), [18, 52, 86, 255]);
  });
  await check('External dimension changes require reacquiring a new handle', async () => {
    await healthy('/external-resize');
    await evaluate('game.width=96'); await click();
    assert.equal(await text(), 'reacquired');
    assert.deepEqual(await pixel(5, 5), [255, 0, 0, 255]);
    assert.deepEqual(errors(), []);
  });
  await check('Resource count release and reused slot versions are bounded', async () => {
    await healthy('/count', 'bounded');
  });
  await check('Aggregate surface pixels and individual dimensions are bounded', async () => {
    await healthy('/pixels', 'pixels-bounded');
  });
  await check('Gradients cannot cross batches or exceed stop limits', async () => {
    await healthy('/gradients', 'gradients-bounded');
  });
  await check('Native arguments styles strings and stack underflow are validated', async () => {
    await healthy('/arguments', 'arguments-bounded');
  });
  for (const [name, mutation] of [
    ['detached node', 'game.remove()'],
    ['adopted node', "document.implementation.createHTMLDocument().adoptNode(game)"],
    ['shadow tree', "document.body.appendChild(document.createElement('div')).attachShadow({mode:'open'}).appendChild(game)"],
    ['customized attribute', "game.setAttribute('is','special-canvas')"],
  ]) {
    await check(`Canvas handle expires for ${name}`, async () => {
      await healthy('/invalid-node');
      await evaluate(mutation); await click();
      assert.equal(await text(), 'expired');
      assert.deepEqual(errors(), []);
    });
  }
  await check('Python does not take over an existing JavaScript context', async () => {
    await healthy('/existing-context');
    await evaluate("game.getContext('2d').fillRect(0,0,10,10)");
    await click(); assert.equal(await text(), 'owned-by-js');
    assert.deepEqual(errors(), []);
  });
  await check('A canvas tainted by external JavaScript expires the Python handle', async () => {
    await healthy('/invalid-node');
    await evaluate(`(async () => {const image=new Image();image.src=${JSON.stringify(imageOrigin + '/image.svg')};await image.decode();game.getContext('2d').drawImage(image,0,0);})()`);
    assert.equal(await evaluate("(() => {try {game.getContext('2d').getImageData(0,0,1,1);return 'clean'} catch(e){return e.name}})()"), 'SecurityError');
    await click(); assert.equal(await text(), 'expired');
    assert.deepEqual(errors(), []);
  });
  await check('Drawing budget latches a fatal stop even when Python catches the error', async () => {
    await navigate('/stop');
    assert.equal(await text(), 'before');
    assert.ok(errors().some(e => /Canvas drawing budget|drawing budget/i.test(e.text)), JSON.stringify(errors()));
  });
  await check('Python-only layered scene animates with measured batch costs', async () => {
    await send('Emulation.setDeviceMetricsOverride', {width: 1024, height: 800,
      deviceScaleFactor: 1, mobile: false});
    await navigate('/sample'); await send('Page.bringToFront');
    await waitFor(async () => /^frames:([1-9]\d*):x:/.test(await text()));
    assert.equal(await evaluate('document.scripts.length'), 0);
    assert.match(await read('metrics'), /commands:7 bytes:903 pixels:\d+ native_ms:/);
    await delay(350);
    assert.deepEqual(errors(), []);
    const screenshot = await send('Page.captureScreenshot', {format: 'png'});
    fs.writeFileSync(`${output}.sample.png`, Buffer.from(screenshot.data, 'base64'));
    return {state: await text(), statistics: await read('metrics'), screenshot: `${output}.sample.png`};
  });
  await check('Trusted keyboard input moves the Python sprite', async () => {
    const before = Number((await text()).split(':')[3]);
    await key('keyDown', 'ArrowRight', 'ArrowRight', 39);
    await waitFor(async () => Number((await text()).split(':')[3]) > before + 10);
    await key('keyUp', 'ArrowRight', 'ArrowRight', 39);
    assert.deepEqual(errors(), []);
  });
  await check('Freeze and resume keep a bounded drawing frame', async () => {
    await send('Page.setWebLifecycleState', {state: 'frozen'});
    assert.equal(await evaluate('document.hidden'), true);
    await delay(250);
    const before = await text();
    await delay(200); assert.equal(await text(), before);
    await send('Page.setWebLifecycleState', {state: 'active'});
    // CDP unfreezes without restoring the visibility changed by freeze.
    const {targetId} = await send('Target.createTarget', {url: 'about:blank'});
    await send('Target.activateTarget', {targetId});
    await send('Page.bringToFront');
    await send('Target.closeTarget', {targetId});
    await waitFor(() => evaluate('!document.hidden'));
    await waitFor(async () => await text() !== before);
    assert.deepEqual(errors(), []);
  });
  await check('Navigation and reload create fresh Python canvas resources', async () => {
    await navigate('/other'); await delay(150); assert.equal(await text(), 'other');
    await navigate('/sample');
    await waitFor(async () => /^frames:/.test(await text()));
    await evaluate('globalThis.reloadMarker = true');
    events.length = 0;
    await send('Page.reload');
    await waitFor(() => events.some(event => event.method === 'Page.loadEventFired'));
    assert.equal(await evaluate('globalThis.reloadMarker'), undefined);
    await waitFor(async () => /^frames:/.test(await text()));
    assert.deepEqual(errors(), []);
  });
  await check('Browser closes normally with cached surfaces and a pending frame', async () => {
    await send('Browser.close'); await waitFor(() => child.exitCode !== null);
    assert.equal(child.exitCode, 0);
  });
  report.passed = true;
} catch (error) {
  report.passed = false;
  report.error = String(error.stack ?? error);
  process.exitCode = 1;
} finally {
  await close();
  await new Promise(resolve => server.close(resolve));
  await new Promise(resolve => imageServer.close(resolve));
  fs.writeFileSync(output, JSON.stringify(report, null, 2) + '\n');
  assert.equal(path.dirname(root), path.resolve(os.tmpdir()));
  assert.ok(path.basename(root).startsWith('pyscript-canvas-'));
  fs.rmSync(root, {recursive: true, force: true, maxRetries: 8, retryDelay: 250});
}
console.log(JSON.stringify({passed: report.passed, cases: report.cases.length, output,
  error: report.error}, null, 2));
