// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Real CPython -> native Blink input. CDP is only the external test driver;
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
const output = path.resolve(option('output', 'out/Debug/pyscript-input-smoke.json'));
const chromeArgs = process.argv.slice(2).filter(arg => arg.startsWith('--chrome-arg='))
    .map(arg => arg.slice('--chrome-arg='.length));
assert.ok(!chromeArgs.some(arg => /no-sandbox|disable-web-security/.test(arg)));
fs.mkdirSync(path.dirname(output), {recursive: true});
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'pyscript-input-'));
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const wrap = source => `<!doctype html><meta charset="utf-8">${source}`;
const makePage = (source, extra = '') => ({body: wrap(
    '<p id="result">ready</p><button id="button">Control</button>' + extra +
    `<pyscript nonce="input">import dom, animation
target = dom.get_element_by_id('result')
document = dom.get_document()
window = dom.get_window()
${source}</pyscript>`)});
const pages = new Map([
  ['/sample', {file: 'chrome/test/data/pyscript/input-state.html',
    headers: {'Content-Security-Policy': "script-src 'nonce-python-input'"}}],
  ['/other', {body: wrap('<p id="result">other</p>')}],
  ['/keys', makePage(`
def key(e):
    dom.set_text(target, '|'.join(map(str, (e.type, e.key, e.code, e.repeat,
        e.is_composing, e.alt_key, e.ctrl_key, e.meta_key, e.shift_key,
        e.target_is_editable, e.target_in_dialog, e.is_trusted))))
dom.add_event_listener(document, 'keydown', key)
dom.add_event_listener(document, 'keyup', key)
`, '<input id="edit"><div id="editable" contenteditable>editable</div>' +
       '<dialog open><button id="inside">dialog</button></dialog>')],
  ['/control', makePage(`
saved = None
count = 0
def expired():
    try: saved.prevent_default()
    except ReferenceError: return 'expired'
    raise AssertionError('old token accepted')
def frame(t): dom.set_text(target, expired() + ':frame')
def callback(e):
    global saved, count
    if saved is not None: assert expired() == 'expired'
    e.prevent_default()
    e.stop_propagation()
    saved = e
    count += 1
    dom.set_text(target, str(count))
    animation.request_frame(frame)
dom.add_event_listener(dom.get_element_by_id('button'), 'click', callback)
`)],
  ...[false, true].map(passive => [`/passive-${passive}`, makePage(`
def callback(e):
    try:
        e.prevent_default()
        dom.set_text(target, 'prevented')
    except PermissionError:
        dom.set_text(target, 'passive')
dom.add_event_listener(dom.get_element_by_id('check'), 'click', callback, passive=${passive ? 'True' : 'False'})
`, '<input id="check" type="checkbox">')]),
  ['/propagation', makePage(`
seen = []
def capture(e):
    seen.append('capture')
    dom.set_text(target, ','.join(seen))
    if e.code == 'KeyS': e.stop_propagation()
def bubble(e):
    seen.append('bubble')
    dom.set_text(target, ','.join(seen))
def first(e):
    dom.set_text(target, 'first')
    e.stop_immediate_propagation()
dom.add_event_listener(document, 'keydown', capture, capture=True)
dom.add_event_listener(document, 'keydown', bubble)
dom.add_event_listener(dom.get_element_by_id('button'), 'click', first)
dom.add_event_listener(dom.get_element_by_id('button'), 'click', lambda e: dom.set_text(target, 'forbidden'))
`)],
  ['/release', makePage(`
button = dom.get_element_by_id('button')
def down(e):
    dom.set_pointer_capture(button, e.pointer_id)
    dom.set_text(target, 'captured')
def move(e):
    if e.buttons:
        dom.release_pointer_capture(button, e.pointer_id)
def lost(e): dom.set_text(target, 'lost:' + str(e.is_trusted))
dom.add_event_listener(button, 'pointerdown', down)
dom.add_event_listener(button, 'pointermove', move)
dom.add_event_listener(button, 'lostpointercapture', lost)
`)],
  ['/synthetic-pointer', makePage(`
def callback(e):
    assert not e.is_trusted
    try: dom.set_pointer_capture(e.current_target, e.pointer_id)
    except RuntimeError: dom.set_text(target, 'invalid:False')
    else: raise AssertionError('synthetic pointer created native active state')
dom.add_event_listener(dom.get_element_by_id('button'), 'pointerdown', callback)
`)],
  ['/page', makePage(`
def state(e):
    page = dom.get_page_state()
    dom.set_text(target, '|'.join(map(str, (page.width, page.height, page.hidden,
        page.has_focus, page.reduced_motion, page.input_epoch))))
dom.add_event_listener(window, 'resize', state)
dom.add_event_listener(window, 'focus', state)
dom.add_event_listener(window, 'blur', state)
dom.add_event_listener(document, 'visibilitychange', state)
state(None)
`)],
  ['/window-pressure', makePage(`
held = False
epoch = dom.get_page_state().input_epoch
def key(e):
    global held
    held = e.type == 'keydown'
def fill(e): print('x' * 3072, end='')
def tick(t):
    global held, epoch
    current = dom.get_page_state().input_epoch
    if current != epoch: held = False
    epoch = current
    dom.set_text(target, str(held) + ':' + str(epoch))
    animation.request_frame(tick)
dom.add_event_listener(document, 'keydown', key)
dom.add_event_listener(document, 'keyup', key)
dom.add_event_listener(dom.get_element_by_id('button'), 'click', fill)
animation.request_frame(tick)
`)],
  ['/fatal', makePage(`
button = dom.get_element_by_id('button')
def down(e):
    dom.set_pointer_capture(button, e.pointer_id)
    dom.set_text(target, 'entered')
    print('x' * 4097)
dom.add_event_listener(button, 'pointerdown', down)
dom.add_event_listener(document, 'keyup', lambda e: dom.set_text(target, 'forbidden'))
`)],
  ['/transfer', makePage(`
button = dom.get_element_by_id('button')
def down(e):
    dom.set_pointer_capture(button, e.pointer_id)
    dom.set_text(target, str(e.pointer_id))
def stop(e): print('x' * 4097)
dom.add_event_listener(button, 'pointerdown', down)
dom.add_event_listener(document, 'keydown', stop)
`, '<button id="other">New capture owner</button>')],
]);
const server = http.createServer((request, response) => {
  const page = pages.get(request.url);
  if (!page) return response.writeHead(404).end();
  response.writeHead(200, {'Content-Type': 'text/html; charset=utf-8', ...page.headers});
  response.end(page.file ? fs.readFileSync(path.resolve(page.file)) : page.body);
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
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
  const value = await send('Runtime.evaluate', {expression, returnByValue: true});
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
const rect = id => evaluate(`(() => {const e=document.getElementById(${JSON.stringify(id)});e.scrollIntoView({block:'center'});const r=e.getBoundingClientRect(); return {x:r.x+r.width/2,y:r.y+r.height/2}})()`);
async function key(type, code, keyValue, virtualKey, extra = {}) {
  await send('Input.dispatchKeyEvent', {type, code, key: keyValue,
    windowsVirtualKeyCode: virtualKey, nativeVirtualKeyCode: virtualKey, ...extra});
}
async function mouse(type, point, extra = {}) {
  // Keep the changed/current button consistent with the pressed-button mask.
  // Omitting it during a left drag can clear pending native mouse capture.
  await send('Input.dispatchMouseEvent', {
    type, ...point, button: extra.buttons & 1 ? 'left' : 'none', ...extra,
  });
}
async function press(id) {
  const point = await rect(id);
  await mouse('mouseMoved', point);
  await mouse('mousePressed', point, {button: 'left', buttons: 1, clickCount: 1});
  return point;
}
async function release(point) {
  await mouse('mouseReleased', point, {button: 'left', buttons: 0, clickCount: 1});
}
async function sample() {
  await navigate('/sample');
  await send('Page.bringToFront');
  await waitFor(async () => await read('result') === '|0');
}
try {
  await launch();
  await check('Python-only sample handles held keys, repeat and keyup', async () => {
    await sample();
    assert.equal(await evaluate('document.scripts.length'), 0);
    await key('keyDown', 'ArrowRight', 'ArrowRight', 39);
    await waitFor(async () => await read('result') === 'ArrowRight|0');
    await key('keyDown', 'ArrowRight', 'ArrowRight', 39, {autoRepeat: true});
    assert.equal(await read('last'), 'keydown:ArrowRight:True:True');
    await key('keyDown', 'Space', ' ', 32, {text: ' '});
    await waitFor(async () => await read('result') === 'ArrowRight,Space|0');
    await key('keyUp', 'ArrowRight', 'ArrowRight', 39);
    await key('keyUp', 'Space', ' ', 32);
    await waitFor(async () => await read('result') === '|0');
    assert.deepEqual(errors(), []);
    const png = await send('Page.captureScreenshot', {format: 'png'});
    fs.writeFileSync(`${output}.sample.png`, Buffer.from(png.data, 'base64'));
  });
  await check('Control keys prevent native scrolling synchronously', async () => {
    await sample();
    await key('keyDown', 'Space', ' ', 32, {text: ' '});
    await delay(200);
    assert.equal(await evaluate('scrollY'), 0);
    await key('keyUp', 'Space', ' ', 32);
  });
  await check('Editable input keeps native typing and default space behavior', async () => {
    await sample();
    await evaluate("document.getElementById('edit').focus()");
    await send('Input.insertText', {text: 'hello'});
    await key('keyDown', 'Space', ' ', 32, {text: ' '});
    await key('keyUp', 'Space', ' ', 32);
    await waitFor(async () => await read('form') === 'input:hello :False');
    assert.equal(await evaluate("document.getElementById('edit').value"), 'hello ');
    assert.equal(await read('result'), '|0');
    assert.deepEqual(errors(), []);
  });
  await check('Real checkbox change reports checked and value snapshots', async () => {
    await sample();
    const point = await press('check'); await release(point);
    await waitFor(async () => await read('form') === 'change:on:True');
  });
  await check('Keyboard snapshots preserve modifiers, repeat and trust', async () => {
    await navigate('/keys');
    await evaluate('document.activeElement.blur()');
    await key('keyDown', 'KeyA', 'A', 65, {modifiers: 8, autoRepeat: true});
    assert.equal(await text(), 'keydown|A|KeyA|True|False|False|False|False|True|False|False|True');
    await key('keyUp', 'KeyA', 'A', 65, {modifiers: 8});
  });
  await check('Contenteditable and open dialogs are identified without node exposure', async () => {
    await navigate('/keys');
    await evaluate("document.getElementById('editable').focus()");
    await key('keyDown', 'ArrowLeft', 'ArrowLeft', 37);
    assert.ok((await text()).endsWith('|True|False|True'));
    await key('keyUp', 'ArrowLeft', 'ArrowLeft', 37);
    await evaluate("document.getElementById('inside').focus()");
    await key('keyDown', 'ArrowLeft', 'ArrowLeft', 37);
    assert.ok((await text()).endsWith('|True|True|True'));
    await key('keyUp', 'ArrowLeft', 'ArrowLeft', 37);
  });
  await check('Script-created keyboard events remain untrusted and preserve composition', async () => {
    await navigate('/keys');
    await evaluate("document.dispatchEvent(new KeyboardEvent('keydown', {code:'KeyX',key:'x',isComposing:true}))");
    assert.equal(await text(), 'keydown|x|KeyX|False|True|False|False|False|False|False|False|False');
  });
  await check('Closed shadow controls conservatively retain native keyboard behavior', async () => {
    await navigate('/keys');
    await evaluate("const host=document.createElement('div');document.body.append(host);const root=host.attachShadow({mode:'closed'});root.innerHTML='<input>';root.firstChild.focus()");
    await key('keyDown', 'ArrowLeft', 'ArrowLeft', 37);
    assert.ok((await text()).endsWith('|True|False|True'));
    await key('keyUp', 'ArrowLeft', 'ArrowLeft', 37);
  });
  await check('Native button space activation remains accessible', async () => {
    await sample();
    await evaluate("globalThis.activated=0;document.getElementById('pad').addEventListener('click',()=>activated++);document.getElementById('pad').focus()");
    await key('keyDown', 'Space', ' ', 32, {text: ' '});
    await key('keyUp', 'Space', ' ', 32);
    assert.equal(await evaluate('activated'), 1);
    assert.equal(await read('result'), '|0');
  });
  await check('Real mouse capture delivers pointerup outside the element', async () => {
    await sample();
    await press('pad');
    await waitFor(async () => await read('result') === '|1');
    const outside = {x: 700, y: 30};
    await mouse('mouseMoved', outside, {buttons: 1});
    await release(outside);
    await waitFor(async () => await read('result') === '|0');
    assert.match(await read('last'), /^(pointerup|lostpointercapture):mouse:\d+:True$/);
    assert.deepEqual(errors(), []);
  });
  await check('Explicit capture release emits native lostpointercapture', async () => {
    await navigate('/release');
    const point = await press('button');
    assert.equal(await text(), 'captured');
    await mouse('mouseMoved', {x: point.x + 2, y: point.y + 2}, {buttons: 1});
    await mouse('mouseMoved', {x: point.x + 3, y: point.y + 3}, {buttons: 1});
    await waitFor(async () => await text() === 'lost:True');
    await release(point);
  });
  await check('Real touch cancellation clears held pointer state', async () => {
    await sample();
    const point = await rect('pad');
    await send('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{...point, id: 5}]});
    await waitFor(async () => await read('result') === '|1');
    await send('Input.dispatchTouchEvent', {type: 'touchCancel', touchPoints: []});
    await waitFor(async () => await read('result') === '|0');
    assert.match(await read('last'), /^(pointercancel|lostpointercapture):touch:\d+:True$/);
    assert.deepEqual(errors(), []);
  });
  await check('Synthetic pointer IDs cannot acquire native capture', async () => {
    await navigate('/synthetic-pointer');
    await evaluate("document.getElementById('button').dispatchEvent(new PointerEvent('pointerdown',{pointerId:123,pointerType:'pen'}))");
    assert.equal(await text(), 'invalid:False');
  });
  await check('Retained event token expires in the next event and animation frame', async () => {
    await navigate('/control');
    await click();
    await waitFor(async () => await text() === 'expired:frame');
    await click();
    await waitFor(async () => await text() === 'expired:frame');
    assert.deepEqual(errors(), []);
  });
  for (const passive of [false, true]) {
    await check(`Passive=${passive} respects native checkbox default behavior`, async () => {
      await navigate(`/passive-${passive}`);
      const point = await press('check'); await release(point);
      assert.equal(await text(), passive ? 'passive' : 'prevented');
      assert.equal(await evaluate("document.getElementById('check').checked"), passive);
      assert.deepEqual(errors(), []);
    });
  }
  await check('Capture and bubble ordering, stop propagation and immediate stop', async () => {
    await navigate('/propagation');
    await evaluate("globalThis.targetKeys=0;document.getElementById('button').addEventListener('keydown',()=>targetKeys++);document.getElementById('button').focus()");
    await key('keyDown', 'KeyA', 'a', 65);
    await key('keyUp', 'KeyA', 'a', 65);
    assert.equal(await text(), 'capture,bubble');
    assert.equal(await evaluate('targetKeys'), 1);
    await key('keyDown', 'KeyS', 's', 83);
    await key('keyUp', 'KeyS', 's', 83);
    assert.equal(await text(), 'capture,bubble,capture');
    assert.equal(await evaluate('targetKeys'), 1);
    await click();
    assert.equal(await text(), 'first');
  });
  await check('Read-only page state follows resize and reduced-motion emulation', async () => {
    await navigate('/page');
    await send('Emulation.setEmulatedMedia', {features: [{name: 'prefers-reduced-motion', value: 'reduce'}]});
    await send('Emulation.setDeviceMetricsOverride', {width: 760, height: 540, deviceScaleFactor: 1, mobile: false});
    await waitFor(async () => (await text()).startsWith('760|540|False|True|True|'));
    const state = await text();
    await send('Emulation.clearDeviceMetricsOverride');
    await send('Emulation.setEmulatedMedia', {features: []});
    return {state};
  });
  await check('Native blur clears held keys while Python callbacks lack focus', async () => {
    await sample();
    await key('keyDown', 'ArrowLeft', 'ArrowLeft', 37);
    await waitFor(async () => await read('result') === 'ArrowLeft|0');
    await send('Emulation.setFocusEmulationEnabled', {enabled: false});
    // Move focus to a second real tab, then restore the original target.
    const other = await send('Target.createTarget', {url: origin + '/other'});
    await send('Target.activateTarget', {targetId: other.targetId});
    await delay(150);
    await send('Page.bringToFront');
    await waitFor(async () => await read('result') === '|0');
    await send('Target.closeTarget', {targetId: other.targetId});
    await key('keyUp', 'ArrowLeft', 'ArrowLeft', 37);
  });
  await check('Hidden/frozen page clears input and resumes without stuck keys', async () => {
    await sample();
    await key('keyDown', 'ArrowRight', 'ArrowRight', 39);
    await waitFor(async () => await read('result') === 'ArrowRight|0');
    await send('Page.setWebLifecycleState', {state: 'frozen'});
    assert.equal(await evaluate('document.hidden'), true);
    await send('Page.setWebLifecycleState', {state: 'active'});
    const other = await send('Target.createTarget', {url: origin + '/other'});
    await send('Target.activateTarget', {targetId: other.targetId});
    await send('Page.bringToFront');
    await waitFor(async () => await read('result') === '|0');
    await send('Target.closeTarget', {targetId: other.targetId});
    await key('keyUp', 'ArrowRight', 'ArrowRight', 39);
    assert.deepEqual(errors(), []);
  });
  await check('Shared window pressure drops keyup and epoch clears stale held state', async () => {
    await navigate('/window-pressure');
    await key('keyDown', 'ArrowLeft', 'ArrowLeft', 37);
    await waitFor(async () => (await text()).startsWith('True:'));
    for (let i = 0; i < 5; ++i) await click();
    await key('keyUp', 'ArrowLeft', 'ArrowLeft', 37);
    await waitFor(() => nativeLogs().some(e => /event budget window is full/.test(e.text)));
    await waitFor(async () => /^False:[1-9]\d*$/.test(await text()));
    return {state: await text()};
  });
  await check('Fatal input limit revokes global listeners and owned pointer capture', async () => {
    await navigate('/fatal');
    const point = await press('button');
    await waitFor(() => errors().some(e => /output was truncated at 4 KiB/.test(e.text)));
    assert.equal(await text(), 'entered');
    assert.equal(await evaluate("document.getElementById('button').hasPointerCapture(1)"), false);
    await key('keyUp', 'ArrowLeft', 'ArrowLeft', 37);
    await release(point);
    assert.equal(await text(), 'entered');
  });
  await check('Navigation while a pointer is captured leaves the next document clean', async () => {
    await sample(); await press('pad');
    await waitFor(async () => await read('result') === '|1');
    await navigate('/other');
    await release({x: 100, y: 100});
    await key('keyUp', 'ArrowRight', 'ArrowRight', 39);
    assert.equal(await text(), 'other');
    await sample();
    assert.equal(await read('result'), '|0');
    assert.deepEqual(errors(), []);
  });
  await check('Cleanup preserves capture transferred to a different native owner', async () => {
    await navigate('/transfer');
    const point = await press('button');
    const id = Number(await text());
    assert.ok(Number.isInteger(id) && id > 0);
    await evaluate(`document.getElementById('other').setPointerCapture(${id})`);
    await key('keyDown', 'KeyA', 'a', 65);
    await waitFor(() => errors().some(e => /output was truncated at 4 KiB/.test(e.text)));
    assert.equal(await evaluate(`document.getElementById('other').hasPointerCapture(${id})`), true);
    await key('keyUp', 'KeyA', 'a', 65);
    await release(point);
  });
  await check('Browser closes normally with input listeners and a pending frame', async () => {
    await send('Browser.close');
    await waitFor(() => child.exitCode !== null);
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
  fs.writeFileSync(output, JSON.stringify(report, null, 2) + '\n');
  assert.equal(path.dirname(root), path.resolve(os.tmpdir()));
  assert.ok(path.basename(root).startsWith('pyscript-input-'));
  fs.rmSync(root, {recursive: true, force: true, maxRetries: 8, retryDelay: 250});
}
console.log(JSON.stringify({passed: report.passed, cases: report.cases.length, output,
  error: report.error}, null, 2));
