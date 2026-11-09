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
const output = path.resolve(option('output', 'out/Debug/pyscript-event-smoke.json'));
const chromeArgs = process.argv.slice(2).filter(arg => arg.startsWith('--chrome-arg='))
    .map(arg => arg.slice('--chrome-arg='.length));
assert.ok(!chromeArgs.some(arg => /no-sandbox|disable-web-security/.test(arg)));
fs.mkdirSync(path.dirname(output), {recursive: true});
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'pyscript-event-'));
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const block = source => `<pyscript>${source}</pyscript>`;
const wrap = body => `<!doctype html><meta charset="utf-8">${body}`;
const scaffold = '<button id="button"><span id="child">Click</span></button><p id="result">0</p>';
const makePage = (source, extra = '') => ({body: wrap(scaffold + extra + block(source))});
const pages = new Map([
  ['/sample', {file: 'chrome/test/data/pyscript/click-events.html',
    headers: {'Content-Security-Policy': "script-src 'nonce-python-click'"}}],
  ['/counter', makePage(`
import dom
count = 0
def clicked(event):
    global count
    count += 1
    assert event.type == 'click'
    dom.set_text(dom.get_element_by_id('result'), str(count) + ':' + str(event.is_trusted))
listener = dom.add_event_listener(dom.get_element_by_id('button'), 'click', clicked)
`)],
  ['/duplicates', makePage(`
import dom
count = 0
def callback(event):
    global count
    count += 1
    dom.set_text(dom.get_element_by_id('result'), str(count))
button = dom.get_element_by_id('button')
for i in range(2):
    dom.add_event_listener(button, 'click', callback)
`)],
  ['/once', makePage(`
import dom
result = dom.get_element_by_id('result')
def fail(event):
    dom.set_text(result, 'once')
    raise ValueError('once-consumed')
dom.add_event_listener(dom.get_element_by_id('button'), 'click', fail, once=True)
`)],
  ['/order', makePage(`
import dom
button = dom.get_element_by_id('button')
result = dom.get_element_by_id('result')
order = ''
def third(event):
    global order
    order += '3'
    dom.set_text(result, order)
def first(event):
    global order, added
    order += '1'
    dom.remove_event_listener(second_handle)
    dom.remove_event_listener(first_handle)
    added = dom.add_event_listener(button, 'click', third)
    dom.set_text(result, order)
def second(event):
    raise AssertionError('removed listener ran')
first_handle = dom.add_event_listener(button, 'click', first)
second_handle = dom.add_event_listener(button, 'click', second)
`)],
  ['/error', makePage(`
import dom
count = 0
def callback(event):
    global count
    count += 1
    dom.set_text(dom.get_element_by_id('result'), str(count))
    raise ValueError('ordinary-click-error')
dom.add_event_listener(dom.get_element_by_id('button'), 'click', callback)
`)],
  ['/timeout', makePage(`
import dom
def callback(event):
    dom.set_text(dom.get_element_by_id('result'), 'entered')
    while True: pass
dom.add_event_listener(dom.get_element_by_id('button'), 'click', callback)
`)],
  ['/reject', makePage(`
import dom
for id in ('svg', 'custom', 'builtin'):
    try:
        dom.add_event_listener(dom.get_element_by_id(id), 'click', lambda event: None)
    except PermissionError:
        pass
    else:
        raise AssertionError('listener target accepted: ' + id)
assert dom.get_element_by_id('shadow') is None
dom.set_text(dom.get_element_by_id('result'), 'rejected')
`, `<svg id="svg"></svg><x-custom id="custom"></x-custom><button is="x-button" id="builtin">B</button>
<div id="host"></div><script>
customElements.define('x-custom', class extends HTMLElement {});
document.querySelector('#host').attachShadow({mode:'open'}).innerHTML='<button id="shadow">S</button>';
</script>`)],
  ['/provenance', {body: wrap(scaffold + '<button id="later">Later</button><button id="blocked">Blocked</button>' +
    `<pyscript nonce="a">
import dom
result = dom.get_element_by_id('result')
def child(event):
    dom.set_text(result, 'child-a')
def parent(event):
    dom.add_event_listener(dom.get_element_by_id('later'), 'click', child)
    dom.set_text(result, 'parent-a')
dom.add_event_listener(dom.get_element_by_id('button'), 'click', parent, once=True)
</pyscript><pyscript nonce="b">
def blocked(event):
    dom.set_text(result, 'forbidden-b')
dom.add_event_listener(dom.get_element_by_id('blocked'), 'click', blocked)
</pyscript>`), headers: {'Content-Security-Policy': "script-src 'nonce-a' 'nonce-b'"}}],
  ['/other', {body: wrap('<p id="other">other</p>')}],
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
const text = () => evaluate('document.querySelector("#result").textContent');
const click = (selector = '#button') => evaluate(`document.querySelector(${JSON.stringify(selector)}).click()`);
try {
  await launch();
  await check('Trusted mouse and keyboard clicks share Python state', async () => {
    await navigate('/counter');
    const rect = await evaluate(`(() => {const r=document.querySelector('#button').getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()`);
    await send('Input.dispatchMouseEvent', {type:'mousePressed',button:'left',clickCount:1,...rect});
    await send('Input.dispatchMouseEvent', {type:'mouseReleased',button:'left',clickCount:1,...rect});
    assert.equal(await text(), '1:True');
    await evaluate('document.querySelector("#button").focus()');
    await send('Input.dispatchKeyEvent', {type:'keyDown',key:'Enter',code:'Enter',text:'\r',unmodifiedText:'\r',windowsVirtualKeyCode:13});
    await send('Input.dispatchKeyEvent', {type:'keyUp',key:'Enter',code:'Enter',windowsVirtualKeyCode:13});
    assert.equal(await text(), '2:True');
    await click('#child');
    assert.equal(await text(), '3:False');
    return {mouse:'trusted',keyboard:'trusted',script:'untrusted'};
  });
  await check('Nonce CSP sample uses no JavaScript and survives source removal', async () => {
    await navigate('/sample');
    await evaluate('document.querySelector("pyscript").remove()');
    await send('HeapProfiler.collectGarbage');
    await click(); await click();
    assert.equal(await text(), '2');
    assert.equal(await evaluate('document.scripts.length'), 0);
    const png = await send('Page.captureScreenshot', {format:'png'});
    fs.writeFileSync(`${output}.sample.png`, Buffer.from(png.data, 'base64'));
  });
  await check('Duplicate registrations remain independent after handles are dropped', async () => {
    await navigate('/duplicates');
    await send('HeapProfiler.collectGarbage');
    await click(); assert.equal(await text(), '2');
    await click(); assert.equal(await text(), '4');
  });
  await check('Once consumes registration even when Python raises', async () => {
    await navigate('/once');
    await click(); await click();
    assert.equal(await text(), 'once');
    assert.equal(nativeLogs().filter(log => log.text.includes('once-consumed')).length, 1);
  });
  await check('Self removal skips later listener and additions wait until next click', async () => {
    await navigate('/order');
    await click(); assert.equal(await text(), '1');
    await click(); assert.equal(await text(), '13');
    assert.ok(!nativeLogs().some(log => log.level === 'error'), JSON.stringify(nativeLogs()));
  });
  await check('Ordinary exceptions retain listeners and include callback source', async () => {
    await navigate('/error');
    await click(); await click();
    assert.equal(await text(), '2');
    const errors = nativeLogs().filter(log => log.text.includes('ordinary-click-error'));
    assert.equal(errors.length, 2);
    assert.ok(errors.every(log => /callback \d+/.test(log.text)));
    return errors;
  });
  await check('Disconnected nodes skip and reconnection resumes without id transfer', async () => {
    await navigate('/counter');
    await evaluate('globalThis.saved=document.querySelector("#button");saved.remove();saved.click()');
    assert.equal(await text(), '0');
    await evaluate('document.body.append(saved);saved.click()');
    assert.equal(await text(), '1:False');
    await evaluate('saved.replaceWith(saved.cloneNode(true));document.querySelector("#button").click()');
    assert.equal(await text(), '1:False');
  });
  await check('Native subscriptions and source metadata do not retain detached DOM nodes', async () => {
    await navigate('/counter');
    await evaluate(`(() => {
      const button=document.querySelector('#button'), source=document.querySelector('pyscript');
      globalThis.buttonWeak=new WeakRef(button);globalThis.sourceWeak=new WeakRef(source);
      button.remove();source.remove();return true;
    })()`);
    // Finish rendering after detaching nodes so pending layout work cannot keep
    // them alive independently of the Python listener and source metadata.
    await send('Runtime.evaluate', {
      expression: 'new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))',
      awaitPromise: true,
    });
    await send('HeapProfiler.collectGarbage');
    assert.equal(await evaluate('buttonWeak.deref() === undefined'), true);
    assert.equal(await evaluate('sourceWeak.deref() === undefined'), true);
  });
  await check('Adopting subscribed node into another document cannot run old Python', async () => {
    await navigate('/counter');
    await evaluate(`globalThis.frame=document.createElement('iframe');document.body.append(frame);
      frame.contentDocument.body.append(document.querySelector('#button'));
      frame.contentDocument.querySelector('#button').click()`);
    assert.equal(await text(), '0');
  });
  await check('Post-registration Shadow DOM and customized built-in are rejected', async () => {
    await navigate('/counter');
    await evaluate(`{const b=document.querySelector('#button');b.setAttribute('is','changed');b.click();}`);
    assert.equal(await text(), '0');
    await evaluate(`{const b=document.querySelector('#button');b.removeAttribute('is');
      const h=document.createElement('div');document.body.append(h);h.attachShadow({mode:'open'}).append(b);b.click()}`);
    assert.equal(await text(), '0');
  });
  await check('New CSP blocks existing callbacks using original source snapshot', async () => {
    await navigate('/counter');
    await click(); assert.equal(await text(), '1:False');
    await evaluate(`document.querySelector('pyscript').remove();
      const m=document.createElement('meta');m.httpEquiv='Content-Security-Policy';m.content="script-src 'none'";document.head.append(m)`);
    await click(); assert.equal(await text(), '1:False');
  });
  await check('Callback-created subscriptions inherit the correct source authorization', async () => {
    await navigate('/provenance');
    await evaluate(`for (const p of document.querySelectorAll('pyscript')) p.remove();
      const m=document.createElement('meta');m.httpEquiv='Content-Security-Policy';m.content="script-src 'nonce-a'";document.head.append(m)`);
    await click(); assert.equal(await text(), 'parent-a');
    await click('#blocked'); assert.equal(await text(), 'parent-a');
    await click('#later'); assert.equal(await text(), 'child-a');
  });
  await check('Disabling page script execution blocks existing callbacks', async () => {
    await navigate('/counter');
    await send('Emulation.setScriptExecutionDisabled', {value:true});
    // CDP evaluation is allowed by the debugger; the host must still check page policy.
    await click(); assert.equal(await text(), '0');
    await send('Emulation.setScriptExecutionDisabled', {value:false});
    await click(); assert.equal(await text(), '1:False');
  });
  await check('Registration rejects SVG, custom elements, customized built-ins and shadow nodes', async () => {
    await navigate('/reject');
    assert.equal(await text(), 'rejected');
  });
  await check('Fatal callback timeout revokes document listeners', async () => {
    await navigate('/timeout');
    await click();
    assert.equal(await text(), 'entered');
    const count = nativeLogs().length;
    assert.ok(nativeLogs().some(log => /100 ms/.test(log.text)), JSON.stringify(nativeLogs()));
    await evaluate('document.querySelector("#result").textContent="stopped"');
    await click();
    assert.equal(await text(), 'stopped');
    assert.equal(nativeLogs().length, count);
  });
  await check('Reload creates fresh callback state and navigation drops old listeners', async () => {
    await navigate('/counter'); await click();
    events.length = 0;
    await send('Page.reload');
    await waitFor(() => events.some(event => event.method === 'Page.loadEventFired'));
    assert.equal(await text(), '0');
    await click(); assert.equal(await text(), '1:False');
    await navigate('/other');
    assert.equal(await evaluate('document.querySelector("#other").textContent'), 'other');
  });
  await check('Browser shuts down normally with active Python listeners', async () => {
    await navigate('/counter'); await click();
    await send('Browser.close');
    await waitFor(() => child.exitCode !== null);
    assert.equal(child.exitCode, 0);
    return {exitCode:child.exitCode};
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
  assert.ok(path.basename(root).startsWith('pyscript-event-'));
  fs.rmSync(root, {recursive: true, force: true, maxRetries: 8, retryDelay: 250});
}
console.log(JSON.stringify({passed: report.passed, cases: report.cases.length, output,
  error: report.error}, null, 2));
