// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Real CPython -> C++ -> Blink tests, with the renderer sandbox enabled.
// Node.js must provide the built-in WebSocket client.
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';

const option = (name, fallback) => process.argv.slice(2)
    .find(arg => arg.startsWith(`--${name}=`))?.slice(name.length + 3) ?? fallback;
const binary = path.resolve(option('chrome-binary', 'out/Debug/chrome.exe'));
const output = path.resolve(option('output', 'out/Debug/pyscript-dom-smoke.json'));
const chromeArgs = process.argv.slice(2).filter(arg => arg.startsWith('--chrome-arg='))
    .map(arg => arg.slice('--chrome-arg='.length));
assert.ok(!chromeArgs.some(arg => /no-sandbox|disable-web-security/.test(arg)));
fs.mkdirSync(path.dirname(output), {recursive: true});
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'pyscript-dom-'));
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const block = source => `<pyscript>${source}</pyscript>`;
const wrap = body => `<!doctype html><meta charset="utf-8">${body}`;
const pages = new Map([
  ['/sample', {file: 'chrome/test/data/pyscript/phase3.html',
    headers: {'Content-Security-Policy': "script-src 'nonce-python-dom'"}}],
  ['/harness', {file: 'chrome/test/data/pyscript/dom-bridge-test.html'}],
  ...['testharness.js', 'testharnessreport.js'].map(name =>
    [`/resources/${name}`, {file: `third_party/blink/web_tests/external/wpt/resources/${name}`,
      type: 'text/javascript'}]),
  ['/isolated', {body: wrap('<p id="result">before</p>' + block(`
import dom
assert not hasattr(dom, 'marker')
assert 'answer' not in globals() and 'handle' not in globals()
dom.set_text(dom.get_element_by_id('result'), 'isolated')
`))}],
  ['/budget', {body: wrap('<p id="result">before</p>' +
    Array.from({length: 8}, (_, index) => block(`
${index === 0 ? 'import dom\nlookup = dom.get_element_by_id' : ''}
# Reduce opcode tracing overhead so the independent time limit is less likely
# to stop this call-count test before all 1,024 native bridge calls complete.
assert list(map(lookup, ['missing'] * 128)) == [None] * 128
${index === 7 ? "print('BUDGET_FIRST', 1024)" : ''}
`)).join('') + block(`
try:
    dom.get_element_by_id('missing')
except RuntimeError:
    pass
else:
    raise AssertionError('document call budget reset between blocks')
`) + block(`
dom.set_text(dom.get_element_by_id('result'), 'forbidden')
print('FORBIDDEN_AFTER_DOM_STOP')
`))}],
  ['/timeout', {body: wrap('<p id="result">before</p>' + block(`
while True: pass
`) + block(`
import dom
dom.set_text(dom.get_element_by_id('result'), 'forbidden')
print('FORBIDDEN_AFTER_TIMEOUT')
`))}],
  ['/output-budget', {body: wrap('<p id="result">before</p>' + block(`
import sys, dom
handle = dom.get_element_by_id('result')
print('x' * 9000)
`) + block(`
# Even clearing tracing and catching the native failure cannot restore DOM.
sys.settrace(None)
try:
    sys.stderr.write('界' * 3000)
except RuntimeError:
    pass
try:
    dom.set_text(handle, 'forbidden')
except ReferenceError:
    pass
`) + block(`print('FORBIDDEN_AFTER_OUTPUT_STOP')`))}],
  ['/diagnostic-loop', {body: wrap(block(`
class Failure(Exception):
    def __str__(self):
        while True: pass
raise Failure()
`) + block(`print('FORBIDDEN_AFTER_DIAGNOSTIC_STOP')`))}],
  ['/diagnostic-budget', {body: wrap(block(`raise ValueError('界' * 10000)`)
    + block(`print('FORBIDDEN_AFTER_DIAGNOSTIC_STOP')`))}],
  ['/console-stop', {body: wrap('<p id="result">before</p>' +
    block(`print('x' * 16330, end='')`) + block(`
import dom
dom.set_text(dom.get_element_by_id('result'), 'forbidden')
`))}],
  ['/parser-diagnostics', {body: wrap(
    block('ignored').replace('<pyscript>', '<pyscript src="unsupported">').repeat(600))}],
  ['/cleanup-loop', {body: wrap('<p id="result">before</p>' + block(`
import dom
dom.set_text(dom.get_element_by_id('result'), 'alive')
class Finalizer:
    def __del__(self):
        while True: pass
value = Finalizer()
value.cycle = value
`))}],
  ['/finalizers', {body: wrap('<p id="result">before</p>' + block(`
import dom, atexit
handle = dom.get_element_by_id('result')
dom.set_text(handle, 'alive')
class Finalizer:
    def __del__(self):
        try:
            dom.set_text(handle, 'forbidden-finalizer')
        except ReferenceError:
            pass
value = Finalizer()
value.cycle = value
atexit.register(lambda: dom.set_text(handle, 'forbidden-atexit'))
`))}],
  ['/other', {body: wrap('<p id="result">other</p>')}],
]);
const requests = [];
const server = http.createServer((request, response) => {
  requests.push(request.url);
  const page = pages.get(request.url);
  if (!page) {
    response.writeHead(404).end();
    return;
  }
  response.writeHead(200, {'Content-Type': page.type ?? 'text/html; charset=utf-8',
    ...page.headers});
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

let child, ws, log;
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
  if (log !== undefined) fs.closeSync(log);
}
async function launch() {
  log = fs.openSync(`${output}.chrome.log`, 'w');
  child = spawn(binary, ['--headless', '--disable-gpu', '--no-first-run',
    '--no-default-browser-check', '--disable-background-networking', '--no-proxy-server',
    '--enable-logging=stderr', '--remote-debugging-port=0', `--user-data-dir=${root}`,
    '--enable-blink-features=PythonScript', `--native-python-allowed-origin=${origin}`,
    ...chromeArgs, 'about:blank'], {windowsHide: true, stdio: ['ignore', log, log]});
  let launchError;
  child.on('error', error => { launchError = error; });
  const port = await waitFor(() => {
    if (launchError) throw launchError;
    assert.equal(child.exitCode, null, `Chrome exited: ${child.exitCode}`);
    const file = path.join(root, 'DevToolsActivePort');
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
  for (const domain of ['Page', 'Runtime', 'Log', 'Network']) await send(`${domain}.enable`);
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
async function navigate(route) {
  events.length = 0;
  const navigation = await send('Page.navigate', {url: origin + route});
  assert.ok(!navigation.errorText, navigation.errorText);
  await waitFor(() => events.some(event => event.method === 'Page.loadEventFired'));
  assert.equal(await evaluate('location.href'), origin + route);
  return nativeLogs();
}
async function check(name, callback) {
  const start = Date.now();
  try {
    const detail = await callback();
    report.cases.push({name, passed: true, elapsedMs: Date.now() - start, detail});
    console.log(`PASS ${name}`);
  } catch (error) {
    report.cases.push({name, passed: false, elapsedMs: Date.now() - start,
      error: String(error.stack ?? error)});
    throw error;
  }
}
try {
  await launch();
  await check('Python-only sample under nonce CSP', async () => {
    const logs = await navigate('/sample');
    assert.ok(!logs.some(entry => entry.level === 'error'), JSON.stringify(logs));
    const state = await evaluate(`({result:document.querySelector('#result').textContent,
      shared:document.querySelector('#shared').textContent,
      literal:document.querySelector('#literal').textContent,
      images:document.images.length, scripts:document.scripts.length})`);
    assert.equal(state.result, 'Python 计算结果：42');
    assert.equal(state.shared, '共享变量与句柄：43');
    assert.equal(state.literal, '<img src="/must-not-load.png"> &amp; 世界');
    assert.equal(state.images, 0);
    assert.equal(state.scripts, 0);
    const png = await send('Page.captureScreenshot', {format: 'png'});
    fs.writeFileSync(`${output}.sample.png`, Buffer.from(png.data, 'base64'));
    return state;
  });
  await check('WPT testharness DOM bridge assertions', async () => {
    const logs = await navigate('/harness');
    const results = await waitFor(() => evaluate('globalThis.domTestResults'));
    assert.equal(results.status, 0, JSON.stringify(results));
    assert.equal(results.tests.length, 9, JSON.stringify(results));
    for (const test of results.tests) assert.equal(test.status, 0, JSON.stringify(test));
    const errors = logs.filter(entry => entry.level === 'error').map(entry => entry.text).join('\n');
    assert.match(errors, /ValueError: expected-dom-error/);
    assert.match(errors, /SyntaxError/);
    assert.match(logs.map(entry => entry.text).join('\n'), /DOM_BRIDGE_FINISHED/);
    return results;
  });
  await check('Same-document navigation does not rerun blocks', async () => {
    events.length = 0;
    await evaluate('location.hash="same-document"');
    await delay(200);
    assert.equal(nativeLogs().length, 0);
    assert.equal(await evaluate('document.querySelector("#result").textContent'), '共享 43');
  });
  await check('Different Document globals and modules are isolated', async () => {
    const logs = await navigate('/isolated');
    assert.ok(!logs.some(entry => entry.level === 'error'), JSON.stringify(logs));
    assert.equal(await evaluate('document.querySelector("#result").textContent'), 'isolated');
    return logs;
  });
  await check('DOM call budget is shared across blocks', async () => {
    const logs = await navigate('/budget');
    const text = logs.map(entry => entry.text).join('\n');
    assert.match(text, /BUDGET_FIRST 1024/);
    assert.match(text, /1024 DOM call budget/);
    assert.doesNotMatch(text, /FORBIDDEN_AFTER_DOM_STOP/);
    assert.equal(await evaluate('document.querySelector("#result").textContent'), 'before');
    return logs;
  });
  await check('Pure Python timeout stops later blocks and reload starts fresh', async () => {
    const logs = await navigate('/timeout');
    const text = logs.map(entry => entry.text).join('\n');
    assert.match(text, /100 ms cooperative execution budget/);
    assert.doesNotMatch(text, /FORBIDDEN_AFTER_TIMEOUT/);
    assert.equal(await evaluate('document.querySelector("#result").textContent'), 'before');
    events.length = 0;
    await send('Page.reload');
    await waitFor(() => events.some(event => event.method === 'Page.loadEventFired'));
    assert.match(nativeLogs().map(entry => entry.text).join('\n'), /100 ms/);
    await navigate('/isolated');
    assert.equal(await evaluate('document.querySelector("#result").textContent'), 'isolated');
    return logs;
  });
  await check('Document output budget revokes DOM even after caught failure', async () => {
    const logs = await navigate('/output-budget');
    const text = logs.map(entry => entry.text).join('\n');
    const bytes = logs.reduce((sum, entry) => sum + Buffer.byteLength(entry.text), 0);
    assert.ok(bytes <= 16384, String(bytes));
    assert.match(text, /output was truncated at 16 KiB/);
    assert.equal((text.match(/output was truncated at 16 KiB/g) ?? []).length, 1);
    assert.doesNotMatch(text, /FORBIDDEN_AFTER_OUTPUT_STOP/);
    assert.equal(await evaluate('document.querySelector("#result").textContent'), 'before');
    return {bytes, entries: logs.length};
  });
  await check('Exception formatting has a cooperative time budget', async () => {
    const logs = await navigate('/diagnostic-loop');
    const text = logs.map(entry => entry.text).join('\n');
    assert.match(text, /100 ms cooperative execution budget/);
    assert.doesNotMatch(text, /FORBIDDEN_AFTER_DIAGNOSTIC_STOP/);
    return logs;
  });
  await check('Oversized exception stops remaining blocks', async () => {
    const logs = await navigate('/diagnostic-budget');
    const text = logs.map(entry => entry.text).join('\n');
    assert.match(text, /output was truncated at 16 KiB/);
    assert.doesNotMatch(text, /FORBIDDEN_AFTER_DIAGNOSTIC_STOP/);
    return logs;
  });
  await check('Parser diagnostics share the document console limit', async () => {
    const logs = await navigate('/parser-diagnostics');
    const bytes = logs.reduce((sum, entry) => sum + Buffer.byteLength(entry.text), 0);
    assert.ok(bytes <= 16384, String(bytes));
    assert.equal(logs.filter(entry => entry.text.includes('output was truncated at 16 KiB')).length, 1);
    return {bytes, entries: logs.length};
  });
  await check('Console truncation stops execution before the next block', async () => {
    const logs = await navigate('/console-stop');
    assert.equal(logs.filter(entry => entry.text.includes('output was truncated at 16 KiB')).length, 1);
    assert.equal(await evaluate('document.querySelector("#result").textContent'), 'before');
    return {bytes: logs.reduce((sum, entry) => sum + Buffer.byteLength(entry.text), 0)};
  });
  await check('Cyclic Python finalizer loop does not block later navigation', async () => {
    for (let i = 0; i < 3; ++i) {
      await navigate('/cleanup-loop');
      assert.equal(await evaluate('document.querySelector("#result").textContent'), 'alive');
      await navigate('/isolated');
      assert.equal(await evaluate('document.querySelector("#result").textContent'), 'isolated');
    }
    return {iterations: 3};
  });
  await check('document.open invalidates the context and never executes written Python', async () => {
    await navigate('/finalizers');
    events.length = 0;
    await evaluate(`document.open(); document.write(${JSON.stringify(wrap(
      '<p id="replacement">replacement</p>' + block('print("FORBIDDEN_WRITTEN")')))});
      document.close();`);
    await waitFor(() => evaluate('document.readyState === "complete"'));
    assert.equal(await evaluate('document.querySelector("#replacement").textContent'), 'replacement');
    assert.ok(!nativeLogs().some(entry => entry.text.includes('FORBIDDEN_WRITTEN')));
    return nativeLogs();
  });
  await check('Python documents cannot return from BFCache', async () => {
    await navigate('/sample');
    await evaluate('globalThis.oldDocumentMarker = true');
    await navigate('/other');
    events.length = 0;
    await evaluate('history.back()');
    await waitFor(() => events.some(event => event.method === 'Page.loadEventFired'));
    assert.equal(await evaluate('location.href'), origin + '/sample');
    assert.equal(await evaluate('globalThis.oldDocumentMarker === undefined'), true);
    assert.equal(await evaluate('document.querySelector("#result").textContent'), 'Python 计算结果：42');
    const failure = events.find(event => event.method === 'Page.backForwardCacheNotUsed');
    assert.ok(failure, JSON.stringify(events.map(event => event.method)));
    assert.ok(failure.params.notRestoredExplanations.some(explanation =>
      explanation.reason === 'Unknown' && explanation.type === 'SupportPending'),
      JSON.stringify(failure));
    await evaluate('globalThis.secondDocumentMarker = true');
    events.length = 0;
    await evaluate('history.forward()');
    await waitFor(() => evaluate(`location.href === ${JSON.stringify(origin + '/other')} &&
      document.readyState === 'complete'`));
    events.length = 0;
    await evaluate('history.back()');
    await waitFor(() => events.some(event => event.method === 'Page.loadEventFired'));
    assert.equal(await evaluate('location.href'), origin + '/sample');
    assert.equal(await evaluate('globalThis.secondDocumentMarker === undefined'), true);
    assert.equal(await evaluate('document.querySelector("#result").textContent'), 'Python 计算结果：42');
    return failure.params;
  });
  await check('Reload creates a new Python document context', async () => {
    await navigate('/isolated');
    await evaluate('globalThis.oldDocumentMarker = true');
    events.length = 0;
    await send('Page.reload');
    await waitFor(() => events.some(event => event.method === 'Page.loadEventFired'));
    assert.equal(await evaluate('globalThis.oldDocumentMarker === undefined'), true);
    assert.equal(await evaluate('document.querySelector("#result").textContent'), 'isolated');
    assert.ok(!nativeLogs().some(entry => entry.level === 'error'), JSON.stringify(nativeLogs()));
  });
  await check('Repeated finalizer teardown and document creation stay responsive', async () => {
    for (let i = 0; i < 20; ++i) {
      await navigate('/finalizers');
      assert.equal(await evaluate('document.querySelector("#result").textContent'), 'alive');
      await navigate('/isolated');
      assert.equal(await evaluate('document.querySelector("#result").textContent'), 'isolated');
    }
    assert.ok(!requests.some(url => url.startsWith('/must-not-load')));
    return {iterations: 20};
  });
  await check('Browser closes normally with a pending cyclic finalizer', async () => {
    await navigate('/cleanup-loop');
    await send('Browser.close');
    await waitFor(() => child.exitCode !== null, 30000);
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
  report.requests = requests;
  fs.writeFileSync(output, JSON.stringify(report, null, 2) + '\n');
  // Validate the exact mkdtemp result before removing the test-owned profile.
  assert.equal(path.dirname(root), path.resolve(os.tmpdir()));
  assert.ok(path.basename(root).startsWith('pyscript-dom-'));
  fs.rmSync(root, {recursive: true, force: true, maxRetries: 8, retryDelay: 250});
}
console.log(JSON.stringify({passed: report.passed, cases: report.cases.length, output,
  error: report.error}, null, 2));
