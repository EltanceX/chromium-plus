// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Phase two: real sandboxed renderer, inline Python, parser and policy checks.
// Run with a modern Node.js providing the built-in WebSocket client.
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';

const option = (name, fallback) => process.argv.slice(2)
    .find(arg => arg.startsWith(`--${name}=`))?.slice(name.length + 3) ?? fallback;
const binary = path.resolve(option('chrome-binary', 'out/Debug/chrome.exe'));
const output = path.resolve(option('output', 'out/Debug/pyscript-smoke.json'));
const chromeArgs = process.argv.slice(2).filter(arg => arg.startsWith('--chrome-arg='))
    .map(arg => arg.slice('--chrome-arg='.length));
assert.ok(!chromeArgs.some(arg => /no-sandbox|disable-web-security/.test(arg)));
fs.mkdirSync(path.dirname(output), {recursive: true});
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'pyscript-smoke-'));
const pages = new Map();
const requests = [];
const result = {binary, sandboxDisabled: false, chromeArgs, cases: []};
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const wrap = body => `<!doctype html><meta charset="utf-8"><title>Python phase two</title>${body}`;
const block = source => `<pyscript>${source}</pyscript>`;
const simpleSource = 'print("POLICY_ALLOWED")';
const sourceHash = createHash('sha256').update(simpleSource).digest('base64');

const server = http.createServer((request, response) => {
  requests.push(request.url);
  if (request.url === '/deferred.js') {
    response.writeHead(200, {'Content-Type': 'text/javascript'});
    setTimeout(() => response.end('console.log("JS_DEFERRED");'), 100);
    return;
  }
  if (request.url === '/pause.js') {
    response.writeHead(200, {'Content-Type': 'text/javascript'});
    setTimeout(() => response.end('// parser paused for preload scanning'), 100);
    return;
  }
  const page = pages.get(request.url);
  if (!page) {
    response.writeHead(404);
    response.end();
    return;
  }
  response.writeHead(200, {'Content-Type': 'text/html; charset=utf-8', ...page.headers});
  if (page.chunks) {
    let index = 0;
    const next = () => {
      response.write(page.chunks[index++]);
      index < page.chunks.length ? setTimeout(next, 30) : response.end();
    };
    next();
  } else {
    response.end(wrap(page.body));
  }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
pages.set('/simple', {body: block(simpleSource)});
pages.set('/frame', {body: block('print("FORBIDDEN_IFRAME")')});
pages.set('/basic', {body: `
<script src="/pause.js"></script><p id="result">unchanged</p>
<pyscript id="first">
answer = 6 * 7
print("Python 世界", answer)
print('literal &amp; <img src="/forbidden.png">')
print('<script src="/forbidden.js"></script>')
print('<link rel="stylesheet" href="/forbidden.css">')
</pyscript>
${block('print("SHARED", answer + 1)')}
${block('raise ValueError("phase-two")')}
${block('if :')}
${block('print("RECOVERED")')}
<template>${block('print("FORBIDDEN_TEMPLATE")')}</template>
<iframe src="/frame"></iframe>
<iframe srcdoc='${block('print("FORBIDDEN_SRCDOC")')}'></iframe>
<script defer src="/deferred.js"></script>
<script>
const first = document.querySelector('#first');
first.textContent = 'print("FORBIDDEN_MUTATION")';
first.remove(); document.body.append(first);
document.body.append(first.cloneNode(true));
const dynamic = document.createElement('pyscript');
dynamic.textContent = 'print("FORBIDDEN_DYNAMIC")'; document.body.append(dynamic);
const fragment = document.createElement('div');
fragment.innerHTML = '<pyscript>print("FORBIDDEN_FRAGMENT")</pyscript>';
document.body.append(fragment);
const parsed = new DOMParser().parseFromString(
    '<pyscript>print("FORBIDDEN_DOMPARSER")</pyscript>', 'text/html');
document.body.append(document.adoptNode(parsed.querySelector('pyscript')));
document.addEventListener('DOMContentLoaded', () => console.log('JS_DCL'));
</script>`});
pages.set('/chunks', {chunks: [wrap('<pyscript>\r'),
  '\nif 1 < 2:\r\n    print("CHUNK 世界 &amp;")\r\n</PySc', 'RiPt>']});
pages.set('/unclosed', {body: '<pyscript>print("FORBIDDEN_UNCLOSED")'});
pages.set('/unsupported', {body: ['src="/forbidden.py"', 'async', 'defer', 'type="python"']
    .map(attr => `<pyscript ${attr}>print("FORBIDDEN_ATTRIBUTE")</pyscript>`).join('')});
pages.set('/write', {body: block('print("FORBIDDEN_ORIGINAL")') + `<script>
document.write('<pyscript>print("FORBIDDEN_WRITE")</pyscript>');
</script>` + block('print("FORBIDDEN_LATER")')});
pages.set('/write-split', {body: `<script>document.write('<py');</script>` +
    'script>print("FORBIDDEN_SPLIT")</pyscript>'});
pages.set('/removed', {body: '<pyscript id="removed">print("FORBIDDEN_REMOVED")</pyscript>' +
    '<script>document.querySelector("#removed").remove();</script>'});
pages.set('/literal-end', {body: '<pyscript>print("</pyscript><p id="after">after</p>'});
pages.set('/limits', {body: block('#' + 'x'.repeat(65536)) +
    Array.from({length: 17}, (_, i) => block(`print("LIMIT_${i}")`)).join('')});
pages.set('/output-limit', {body: block('print("x" * 20000)')});
pages.set('/off', {body: '<pyscript id="python"><b id="nested">text</b>&amp;</pyscript>'});
for (const [name, policy] of Object.entries({
  none: "script-src 'none'",
  elem: "script-src 'unsafe-inline'; script-src-elem 'none'",
  fallback: "default-src 'none'",
  nonce: "script-src 'nonce-allowed'",
  hash: `script-src 'sha256-${sourceHash}'`,
  sandbox: "sandbox allow-same-origin; script-src 'unsafe-inline'",
})) {
  pages.set(`/csp-${name}`, {body: name === 'nonce' ?
    '<pyscript nonce="wrong">print("FORBIDDEN_NONCE")</pyscript>' +
    `<pyscript nonce="allowed">${simpleSource}</pyscript>` : block(simpleSource),
    headers: {'Content-Security-Policy': policy}});
}
pages.set('/csp-report', {body: block(simpleSource),
  headers: {'Content-Security-Policy-Report-Only': "script-src 'none'"}});
pages.set('/nonce-dangling', {body:
  `<pyscript nonce="allowed" data-x="<pyscript">print("FORBIDDEN_DANGLING")</pyscript>` +
  `<pyscript nonce="allowed" nonce="other">print("FORBIDDEN_DUPLICATE")</pyscript>`,
  headers: {'Content-Security-Policy': "script-src 'nonce-allowed'"}});

async function waitFor(predicate, timeout = 30000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    const value = await predicate();
    if (value) return value;
    await delay(50);
  }
  throw new Error(`Condition timed out after ${timeout} ms`);
}

async function launch(name, extraArgs) {
  const profile = path.join(root, name);
  fs.mkdirSync(profile);
  const log = fs.openSync(`${output}.${name}.chrome.log`, 'w');
  const child = spawn(binary, ['--headless', '--disable-gpu', '--no-first-run',
    '--no-default-browser-check', '--disable-background-networking',
    '--no-proxy-server', '--remote-debugging-port=0', '--enable-logging=stderr',
    `--user-data-dir=${profile}`, ...chromeArgs, ...extraArgs, 'about:blank'],
    {windowsHide: true, stdio: ['ignore', log, log]});
  let launchError;
  child.on('error', error => { launchError = error; });
  let ws;
  const pending = new Map();
  const events = [];
  let id = 0;
  async function close() {
    ws?.close();
    if (child.exitCode === null && child.pid) {
      if (process.platform === 'win32') {
        spawnSync('taskkill', ['/PID', String(child.pid), '/T', '/F'], {windowsHide: true});
      } else {
        child.kill('SIGKILL');
      }
      await Promise.race([new Promise(resolve => child.once('close', resolve)), delay(5000)]);
    }
    fs.closeSync(log);
  }
  try {
    const port = await waitFor(() => {
      if (launchError) throw launchError;
      if (child.exitCode !== null) throw new Error(`Chrome exited: ${child.exitCode}`);
      const portFile = path.join(profile, 'DevToolsActivePort');
      return fs.existsSync(portFile) && fs.readFileSync(portFile, 'utf8').split('\n')[0];
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
    const send = (method, params = {}) => new Promise((resolve, reject) => {
      const requestId = ++id;
      const timer = setTimeout(() => {
        pending.delete(requestId);
        reject(new Error(`CDP timed out: ${method}`));
      }, 20000);
      pending.set(requestId, message => {
        clearTimeout(timer);
        message.error ? reject(new Error(JSON.stringify(message.error))) : resolve(message.result);
      });
      ws.send(JSON.stringify({id: requestId, method, params}));
    });
    const evaluate = async expression => {
      const value = await send('Runtime.evaluate', {expression, returnByValue: true});
      assert.ok(!value.exceptionDetails, JSON.stringify(value.exceptionDetails));
      return value.result.value;
    };
    await send('Page.enable');
    await send('Runtime.enable');
    await send('Log.enable');
    await send('Network.enable');
    return {send, evaluate, events, close, async navigate(route) {
      events.length = 0;
      const navigation = await send('Page.navigate', {url: origin + route});
      assert.ok(!navigation.errorText, navigation.errorText);
      await waitFor(() => events.some(event => event.method === 'Page.loadEventFired'));
      const state = await evaluate(`({url:location.href, ready:document.readyState,
        chromiumPlus:globalThis.chromiumPlus, count:document.querySelectorAll('pyscript').length,
        element:document.querySelector('pyscript')?.constructor.name,
        display:document.querySelector('pyscript') && getComputedStyle(document.querySelector('pyscript')).display,
        nested:!!document.querySelector('#nested'), result:document.querySelector('#result')?.textContent})`);
      assert.equal(state.url, origin + route);
      assert.equal(state.ready, 'complete');
      return {state, logs: events.filter(event => event.method === 'Log.entryAdded')
        .map(event => event.params.entry),
        timeline: events.flatMap(event => event.method === 'Log.entryAdded' ?
          [{kind: 'log', ...event.params.entry}] : event.method === 'Runtime.consoleAPICalled' ?
          [{kind: 'js', text: event.params.args.map(arg => arg.value).join(' ')}] : [])};
    }};
  } catch (error) {
    await close();
    throw error;
  }
}

const native = page => page.logs.filter(entry => entry.source === 'other');
const text = page => native(page).map(entry => entry.text).join('\n');
const assertNoPython = page => assert.equal(native(page).filter(entry => entry.level === 'info').length, 0,
    JSON.stringify(page.logs));
async function check(name, callback) {
  const start = Date.now();
  try {
    const detail = await callback();
    result.cases.push({name, passed: true, elapsedMs: Date.now() - start, detail});
    console.log(`PASS ${name}`);
  } catch (error) {
    result.cases.push({name, passed: false, elapsedMs: Date.now() - start,
      error: String(error.stack ?? error)});
    throw error;
  }
}

let browser;
try {
  browser = await launch('enabled', ['--enable-blink-features=PythonScript',
    `--native-python-allowed-origin=${origin}`]);
  await check('sandboxed Python, shared globals, immutable source, errors and DCL order', async () => {
    const page = await browser.navigate('/basic');
    assert.equal(page.state.chromiumPlus, '0.0.1');
    assert.equal(page.state.element, 'HTMLElement');
    assert.equal(page.state.display, 'none');
    assert.equal(page.state.result, 'unchanged');
    assert.match(text(page), /Python 世界 42/);
    assert.match(text(page), /SHARED 43/);
    assert.match(text(page), /ValueError: phase-two/);
    assert.match(text(page), /SyntaxError/);
    assert.match(text(page), /RECOVERED/);
    assert.doesNotMatch(text(page), /FORBIDDEN_/);
    const first = page.timeline.findIndex(entry => entry.text.includes('Python 世界'));
    const last = page.timeline.findIndex(entry => entry.text.includes('RECOVERED'));
    const deferred = page.timeline.findIndex(entry => entry.kind === 'js' && entry.text === 'JS_DEFERRED');
    const dcl = page.timeline.findIndex(entry => entry.kind === 'js' && entry.text === 'JS_DCL');
    assert.ok(deferred >= 0 && first > deferred && last > first && dcl > last,
        JSON.stringify(page.timeline));
    assert.ok(!requests.some(url => url.startsWith('/forbidden')));
    return page;
  });
  await check('network chunks, CRLF normalization and mixed-case closing tag', async () => {
    const page = await browser.navigate('/chunks');
    assert.match(text(page), /CHUNK 世界 &amp;/);
    return page;
  });
  for (const route of ['/unclosed', '/unsupported', '/write', '/write-split', '/removed']) {
    await check(`inert input ${route}`, async () => {
      const page = await browser.navigate(route);
      assertNoPython(page);
      assert.ok(!requests.includes('/forbidden.py'));
      return page;
    });
  }
  await check('literal closing tag ends RAWTEXT and reports Python syntax error', async () => {
    const page = await browser.navigate('/literal-end');
    assert.match(text(page), /SyntaxError/);
    assert.equal(await browser.evaluate('document.querySelector("#after").textContent'), 'after');
    return page;
  });
  await check('64 KiB source and 16 block limits', async () => {
    const page = await browser.navigate('/limits');
    assert.match(text(page), /source exceeds 64 KiB/);
    assert.match(text(page), /At most 16 Python blocks/);
    assert.match(text(page), /LIMIT_15/);
    assert.doesNotMatch(text(page), /LIMIT_16/);
    return page;
  });
  await check('bounded output diagnostic', async () => {
    const page = await browser.navigate('/output-limit');
    assert.match(text(page), /output was truncated at 16 KiB/);
    return {logs: page.logs.map(entry => ({...entry, text: entry.text.slice(0, 200)}))};
  });
  for (const policy of ['none', 'elem', 'fallback', 'sandbox']) {
    await check(`CSP ${policy} blocks Python`, async () => {
      const page = await browser.navigate(`/csp-${policy}`);
      assertNoPython(page);
      return page;
    });
  }
  for (const policy of ['nonce', 'hash', 'report']) {
    await check(`CSP ${policy} allows matching Python source`, async () => {
      const page = await browser.navigate(`/csp-${policy}`);
      assert.match(text(page), /POLICY_ALLOWED/);
      assert.doesNotMatch(text(page), /FORBIDDEN_NONCE/);
      if (policy === 'report') assert.ok(page.logs.some(entry => entry.source === 'security'));
      return page;
    });
  }
  await check('dangling-markup and duplicate attributes cannot borrow a nonce', async () => {
    const page = await browser.navigate('/nonce-dangling');
    assertNoPython(page);
    return page;
  });
  await check('disabled page scripting blocks Python', async () => {
    await browser.send('Emulation.setScriptExecutionDisabled', {value: true});
    try {
      const page = await browser.navigate('/simple');
      assertNoPython(page);
      return page;
    } finally {
      await browser.send('Emulation.setScriptExecutionDisabled', {value: false});
    }
  });
  await check('navigation creates fresh Python globals', async () => {
    pages.set('/isolated', {body: block('print("ISOLATED", "answer" in globals())')});
    const page = await browser.navigate('/isolated');
    assert.match(text(page), /ISOLATED False/);
    return page;
  });
  await check('same-document navigation does not rerun Python', async () => {
    await browser.navigate('/simple');
    browser.events.length = 0;
    await browser.evaluate('location.hash="fragment"');
    await delay(150);
    assert.equal(browser.events.filter(event => event.method === 'Log.entryAdded' &&
      event.params.entry.source === 'other').length, 0);
  });
  await browser.close();
  browser = undefined;
  for (const [name, args] of [
    ['default-off', [`--native-python-allowed-origin=${origin}`]],
    ['missing-origin', ['--enable-blink-features=PythonScript']],
    ['wrong-origin', ['--enable-blink-features=PythonScript',
      '--native-python-allowed-origin=http://127.0.0.1:1']],
    ['malformed-origin', ['--enable-blink-features=PythonScript',
      `--native-python-allowed-origin=${origin}/not-an-origin`]],
    ['non-loopback-origin', ['--enable-blink-features=PythonScript',
      '--native-python-allowed-origin=https://example.com']],
  ]) {
    browser = await launch(name, args);
    await check(name, async () => {
      const page = await browser.navigate('/simple');
      assertNoPython(page);
      if (name === 'default-off') {
        const off = await browser.navigate('/off');
        assert.equal(off.state.element, 'HTMLUnknownElement');
        assert.equal(off.state.display, 'inline');
        assert.equal(off.state.nested, true);
        return off;
      }
      return page;
    });
    await browser.close();
    browser = undefined;
  }
  result.passed = true;
} catch (error) {
  result.passed = false;
  result.error = String(error.stack ?? error);
  process.exitCode = 1;
} finally {
  await browser?.close();
  await new Promise(resolve => server.close(resolve));
  result.requests = requests;
  fs.writeFileSync(output, JSON.stringify(result, null, 2) + '\n');
  // Only remove the exact directory allocated by this test's mkdtemp call.
  fs.rmSync(root, {recursive: true, force: true, maxRetries: 8, retryDelay: 250});
}
console.log(JSON.stringify({passed: result.passed, cases: result.cases.length, output,
  error: result.error}, null, 2));
