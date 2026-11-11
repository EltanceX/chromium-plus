// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Real CPython -> native Blink rAF. CDP is only the external test driver;
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
const output = path.resolve(option('output', 'out/Debug/pyscript-animation-smoke.json'));
const chromeArgs = process.argv.slice(2).filter(arg => arg.startsWith('--chrome-arg='))
    .map(arg => arg.slice('--chrome-arg='.length));
assert.ok(!chromeArgs.some(arg => /no-sandbox|disable-web-security/.test(arg)));
fs.mkdirSync(path.dirname(output), {recursive: true});
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'pyscript-animation-'));
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const block = source => `<pyscript nonce="frame">${source}</pyscript>`;
const wrap = body => `<!doctype html><meta charset="utf-8">${body}`;
const makePage = (source, extra = '') => ({body: wrap(
    '<p id="result">0</p><button id="button">Control</button>' + extra + block(`
import animation, dom
target = dom.get_element_by_id('result')
${source}`))});
const pages = new Map([
  ['/sample', {file: 'chrome/test/data/pyscript/animation-frames.html',
    headers: {'Content-Security-Policy': "script-src 'nonce-python-animation'"}}],
  ['/other', {body: wrap('<p id="result">other</p>')}],
  ['/parent', {body: wrap('<iframe id="child" src="/sample"></iframe>')}],
  ['/cancel', makePage(`
count = 0
def tick(timestamp):
    global count
    count += 1
    dom.set_text(target, str(count))
    if count == 1:
        animation.request_frame(tick)
        animation.cancel_frame(first)
        animation.cancel_frame(first)
cancelled = animation.request_frame(lambda timestamp: dom.set_text(target, 'forbidden'))
animation.cancel_frame(cancelled)
first = animation.request_frame(tick)
try:
    animation.request_frame(tick)
except RuntimeError:
    pass
else:
    raise AssertionError('duplicate accepted')
`)],
  ['/once-error', makePage(`
count = 0
def tick(timestamp):
    global count
    count += 1
    dom.set_text(target, str(count))
    raise ValueError('one-shot-frame')
animation.request_frame(tick)
dom.add_event_listener(dom.get_element_by_id('button'), 'click', lambda event: animation.request_frame(tick))
`)],
  ['/next-error', makePage(`
count = 0
def tick(timestamp):
    global count
    count += 1
    dom.set_text(target, str(count))
    if count < 2:
        animation.request_frame(tick)
    raise ValueError('next-frame-survives')
animation.request_frame(tick)
`)],
  ...[
    ['/timeout', 'while True: pass', /16 ms cooperative execution budget/],
    // Debug native bridge work can exhaust 16 ms before reaching call 129.
    // Exact call-count boundaries are checked with an injected clock and in
    // Blink unit tests. This browser case requires the first fatal guard.
    ['/dom-stop', 'list(map(animation.cancel_frame, [request] * 129))',
      /128 DOM call budget|16 ms cooperative execution budget/],
    ['/output-stop', "print('x' * 4097)", /output was truncated at 4 KiB/],
    ['/diagnostic-stop', "raise ValueError('x' * 5000)", /output was truncated at 4 KiB/],
  ].map(([route, statement, expected]) => [route, {...makePage(`
def tick(timestamp):
    dom.set_text(target, 'entered')
    animation.request_frame(tick)
    ${statement}
request = animation.request_frame(tick)
dom.add_event_listener(dom.get_element_by_id('button'), 'click', lambda event: dom.set_text(target, 'forbidden'))
`), expected}]),
  ['/output-window', makePage(`
count = 0
times = []
def tick(timestamp):
    global count
    count += 1
    times.append(int(timestamp))
    dom.set_text(target, str(count) + ':' + ','.join(map(str, times)))
    if count < 6:
        print('x' * 3072, end='')
        animation.request_frame(tick)
animation.request_frame(tick)
`)],
  ['/dom-window', makePage(`
fills = 0
lookup = dom.get_element_by_id
def tick(timestamp):
    dom.set_text(target, 'resumed:' + str(int(timestamp)))
def fill(event):
    global fills
    assert list(map(lookup, ['missing'] * 120)) == [None] * 120
    fills += 1
    if fills == 8:
        animation.request_frame(tick)
dom.add_event_listener(dom.get_element_by_id('button'), 'click', fill)
`)],
  ['/shared-window', makePage(`
def tick(timestamp): dom.set_text(target, 'frame')
fills = 0
def fill(event):
    global fills
    fills += 1
    print('x' * 3072, end='')
    if fills == 5:
        animation.request_frame(tick)
def start(event): animation.request_frame(tick)
dom.add_event_listener(dom.get_element_by_id('button'), 'click', fill)
dom.add_event_listener(dom.get_element_by_id('start'), 'click', start)
`, '<button id="start">Start</button>')],
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
const counter = async () => (await text()).split(':').map(Number);
const errors = () => nativeLogs().filter(entry => entry.level === 'error');
try {
  await launch();
  await check('Python-only nonce sample updates and cancels native frames', async () => {
    await navigate('/sample');
    await waitFor(async () => (await counter())[0] >= 10);
    const first = await counter();
    await delay(250);
    const second = await counter();
    assert.ok(second[0] > first[0] && second[1] > first[1]);
    assert.equal(await evaluate('document.scripts.length'), 0);
    await click('stop');
    const stopped = await text();
    await delay(250);
    assert.equal(await text(), stopped);
    await click('start'); await click('start');
    await waitFor(async () => (await counter())[0] > second[0] + 5);
    assert.deepEqual(errors(), []);
    const png = await send('Page.captureScreenshot', {format: 'png'});
    fs.writeFileSync(`${output}.sample.png`, Buffer.from(png.data, 'base64'));
    return {first, second, stopped};
  });
  await check('Duplicate, repeated cancellation and consumed handles preserve next frame', async () => {
    await navigate('/cancel');
    await waitFor(async () => await text() === '2');
    await delay(200);
    assert.equal(await text(), '2');
    assert.deepEqual(errors(), []);
  });
  await check('One-shot errors consume the old request and allow a new request', async () => {
    await navigate('/once-error');
    await waitFor(async () => await text() === '1');
    await delay(150);
    assert.equal(errors().filter(entry => entry.text.includes('one-shot-frame')).length, 1);
    await click();
    await waitFor(async () => await text() === '2');
    assert.equal(errors().filter(entry => entry.text.includes('one-shot-frame')).length, 2);
  });
  await check('A next frame requested before an ordinary error survives', async () => {
    await navigate('/next-error');
    await waitFor(async () => await text() === '2');
    const logs = errors();
    assert.equal(logs.length, 2);
    assert.ok(logs.every(entry => /pyscript frame \d+/.test(entry.text) &&
        entry.text.includes('next-frame-survives') && entry.text.includes('#pyscript-1')));
  });
  for (const route of ['/timeout', '/dom-stop', '/output-stop', '/diagnostic-stop']) {
    await check(`Fatal frame limit revokes frames and click listeners: ${route}`, async () => {
      await navigate(route);
      await waitFor(() => errors().length > 0);
      assert.ok(errors().some(entry => pages.get(route).expected.test(entry.text)),
          JSON.stringify(errors()));
      assert.equal(await text(), 'entered');
      await delay(1100);
      await click();
      assert.equal(await text(), 'entered');
      assert.equal(errors().length, 1);
      return errors();
    });
  }
  await check('Frame output window retains one request and recovers with a fresh timestamp', async () => {
    await navigate('/output-window');
    await waitFor(async () => {
      assert.deepEqual(errors(), []);
      return (await text()).startsWith('6:');
    });
    const times = (await text()).split(':')[1].split(',').map(Number);
    assert.equal(times.length, 6);
    const gaps = times.slice(1).map((value, index) => value - times[index]);
    assert.ok(Math.max(...gaps) >= 500, JSON.stringify({times, gaps}));
    assert.ok(gaps.every(gap => gap > 0), JSON.stringify(gaps));
    assert.deepEqual(errors(), []);
    return {times, gaps};
  });
  await check('Click DOM charges defer one pending frame until the shared window recovers', async () => {
    await navigate('/dom-window');
    await evaluate("for(let i=0;i<8;i++)document.getElementById('button').click()");
    assert.deepEqual(errors(), []);
    assert.equal(await text(), '0');
    await delay(200);
    assert.equal(await text(), '0');
    await waitFor(async () => {
      assert.deepEqual(errors(), []);
      return (await text()).startsWith('resumed:');
    });
    return {clickDomCalls: 960, frameRequests: 1, result: await text()};
  });
  await check('Click output and frames share the same window', async () => {
    await navigate('/shared-window');
    await evaluate("for(let i=0;i<5;i++)document.getElementById('button').click()");
    // The fifth admitted click installs the pending frame. A new start click
    // cannot enter during pressure, and neither can the scheduled frame yet.
    await click('start');
    assert.equal(await text(), '0');
    await delay(200);
    assert.equal(await text(), '0');
    await waitFor(async () => await text() === 'frame');
    assert.deepEqual(errors(), []);
  });
  await check('Source tag removal and GC retain copied frame authorization', async () => {
    await navigate('/sample');
    await waitFor(async () => (await counter())[0] >= 3);
    await evaluate("globalThis.sourceWeak=new WeakRef(document.querySelector('pyscript'));document.querySelector('pyscript').remove()");
    await delay(100);
    await send('HeapProfiler.collectGarbage');
    assert.equal(await evaluate('sourceWeak.deref() === undefined'), true);
    const before = (await counter())[0];
    await waitFor(async () => (await counter())[0] > before + 3);
    assert.deepEqual(errors(), []);
  });
  await check('Late CSP revokes the pending frame and all Python listeners', async () => {
    await navigate('/sample');
    await waitFor(async () => (await counter())[0] >= 3);
    await evaluate(`const meta=document.createElement('meta');meta.httpEquiv='Content-Security-Policy';meta.content="script-src 'none'";document.head.append(meta)`);
    await delay(150);
    const stopped = await text();
    await delay(200); await click('start');
    assert.equal(await text(), stopped);
  });
  await check('Script execution policy revocation cannot revive old frames', async () => {
    await navigate('/sample');
    await waitFor(async () => (await counter())[0] >= 3);
    await send('Emulation.setScriptExecutionDisabled', {value: true});
    await delay(200);
    await send('Emulation.setScriptExecutionDisabled', {value: false});
    const stopped = await text();
    await delay(200); await click('start');
    assert.equal(await text(), stopped);
  });
  await check('Frozen page resumes with current timestamps without old-frame bursts', async () => {
    await navigate('/sample');
    await waitFor(async () => (await counter())[0] >= 3);
    await send('Page.setWebLifecycleState', {state: 'frozen'});
    assert.equal(await evaluate('document.hidden'), true);
    const frozen = await counter();
    await delay(1200);
    assert.deepEqual(await counter(), frozen);
    await send('Page.setWebLifecycleState', {state: 'active'});
    // CDP's freeze calls WasHidden; active only unfreezes. Switch tabs and
    // reactivate the original tab to restore real visibility as well.
    const {targetId} = await send('Target.createTarget', {url: 'about:blank'});
    await send('Target.activateTarget', {targetId});
    await send('Page.bringToFront');
    await send('Target.closeTarget', {targetId});
    await waitFor(() => evaluate('!document.hidden'));
    await waitFor(async () => (await counter())[0] > frozen[0]);
    const resumed = await counter();
    assert.ok(resumed[1] - frozen[1] >= 1000, JSON.stringify({frozen, resumed}));
    assert.ok(resumed[0] - frozen[0] < 15, JSON.stringify({frozen, resumed}));
    return {frozen, resumed};
  });
  await check('Fragment navigation continues and full navigation starts a fresh context', async () => {
    await navigate('/sample');
    await waitFor(async () => (await counter())[0] >= 5);
    const before = (await counter())[0];
    await evaluate("location.hash='frame'");
    await waitFor(async () => (await counter())[0] > before + 3);
    await navigate('/other');
    await delay(200);
    assert.equal(await text(), 'other');
    assert.deepEqual(errors(), []);
    await navigate('/sample');
    await waitFor(async () => (await counter())[0] >= 3);
    await evaluate('globalThis.reloadMarker = true');
    events.length = 0;
    await send('Page.reload');
    await waitFor(() => events.some(event => event.method === 'Page.loadEventFired'));
    assert.equal(await evaluate('globalThis.reloadMarker'), undefined);
    await waitFor(async () => (await counter())[0] >= 3);
  });
  await check('document.open revokes the frame before replacement markup', async () => {
    await navigate('/sample');
    await waitFor(async () => (await counter())[0] >= 3);
    await evaluate("document.open();document.write('<p id=\"result\">written</p>');document.close()");
    await delay(200);
    assert.equal(await text(), 'written');
    assert.deepEqual(errors(), []);
  });
  await close();
  await launch(true);
  await check('Detached child frame stops its old counter and replacement is isolated', async () => {
    await navigate('/parent');
    await waitFor(() => evaluate("document.querySelector('#child').contentDocument?.querySelector('#result')?.textContent.split(':')[0] >= 3"));
    await evaluate("globalThis.old=document.querySelector('#child').contentDocument;document.querySelector('#child').remove()");
    const stopped = await evaluate("old.querySelector('#result').textContent");
    await delay(200);
    assert.equal(await evaluate("old.querySelector('#result').textContent"), stopped);
    await evaluate("const frame=document.createElement('iframe');frame.id='child';frame.src='/sample';document.body.append(frame)");
    await waitFor(() => evaluate("document.querySelector('#child').contentDocument?.querySelector('#result')?.textContent.split(':')[0] >= 3"));
    assert.equal(await evaluate("old.querySelector('#result').textContent"), stopped);
    assert.deepEqual(errors(), []);
  });
  await check('Browser closes normally with a pending Python animation frame', async () => {
    await navigate('/sample');
    await waitFor(async () => (await counter())[0] >= 3);
    await send('Browser.close');
    await waitFor(() => child.exitCode !== null);
    assert.equal(child.exitCode, 0);
    return {exitCode: child.exitCode};
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
  assert.ok(path.basename(root).startsWith('pyscript-animation-'));
  fs.rmSync(root, {recursive: true, force: true, maxRetries: 8, retryDelay: 250});
}
console.log(JSON.stringify({passed: report.passed, cases: report.cases.length, output,
  error: report.error}, null, 2));
