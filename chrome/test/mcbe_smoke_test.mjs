// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Standalone navigation regression test, using Node's built-in UDP and CDP.
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import dgram from 'node:dgram';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';

const args = process.argv.slice(2);
const option = (name, fallback) =>
    args.find(arg => arg.startsWith(`--${name}=`))?.slice(name.length + 3) ?? fallback;
const binary = path.resolve(option('chrome-binary', 'out/Debug/chrome.exe'));
const output = path.resolve(option('output', 'out/Debug/mcbe-smoke-results.json'));
const chromeArgs = args.filter(arg => arg.startsWith('--chrome-arg='))
    .map(arg => arg.slice('--chrome-arg='.length));
const temporaryRoot = path.resolve(os.tmpdir());
const profile = fs.mkdtempSync(path.join(temporaryRoot, 'mcbe-smoke-'));
const logPath = `${output}.chrome.log`;
fs.mkdirSync(path.dirname(output), {recursive: true});
const log = fs.openSync(logPath, 'w');
const results = [];
const sockets = [];
let browser;
let ws;

async function waitFor(predicate, timeout = 10000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    try {
      const value = await predicate();
      if (value) return value;
    } catch (error) {
      // Cross-process/history navigation can replace the CDP execution context.
      if (!/Inspected target navigated or closed|Execution context was destroyed|Cannot find context/.test(error.message)) {
        throw error;
      }
    }
    await delay(50);
  }
  throw new Error(`Condition timed out after ${timeout} ms`);
}

async function check(name, callback) {
  const filter = option('filter', '');
  // Always run the initial navigation, which establishes the baseline page.
  if (filter && results.length && !name.includes(filter)) return;
  const start = Date.now();
  try {
    const detail = await callback();
    results.push({name, passed: true, elapsedMs: Date.now() - start, detail});
    console.log(`PASS ${name}`);
  } catch (error) {
    results.push({name, passed: false, elapsedMs: Date.now() - start,
                  error: String(error.stack ?? error)});
    throw error;
  }
}

const magic = Buffer.from('00ffff00fefefefefdfdfdfd12345678', 'hex');
const guid = 18446744073709551615n;
const title = '测试 <img src=x onerror="globalThis.mcbeInjected=1"> & §aBedrock';
function pong(ping, text) {
  const motd = Buffer.from(text);
  const packet = Buffer.alloc(35 + motd.length);
  packet[0] = 0x1c;
  ping.copy(packet, 1, 1, 9);
  packet.writeBigUInt64BE(guid, 9);
  magic.copy(packet, 17);
  packet.writeUInt16BE(motd.length, 33);
  motd.copy(packet, 35);
  return packet;
}

async function fakeServer(family = 'udp4', host = '127.0.0.1', port = 0) {
  const socket = dgram.createSocket(family);
  sockets.push(socket);
  const state = {mode: 'success', count: 0, validPings: 0};
  socket.on('message', (ping, remote) => {
    ++state.count;
    if (ping.length !== 33 || ping[0] !== 0x01 ||
        !ping.subarray(9, 25).equals(magic)) return;
    ++state.validPings;
    if (state.mode === 'silent' ||
        (state.mode === 'drop-first' && state.count === 1)) return;
    const advertisedGuid = state.mode === 'different-guid' ? 42n : guid;
    const text = state.mode === 'minimal' ? 'MCPE;Minimal;1;v1;0;10' :
        `MCPE;${title};944;1.26.test;12;100;${advertisedGuid};副标题;Survival;1;19132;19133;`;
    const packet = pong(ping, text);
    if (state.mode === 'invalid') packet[17] ^= 1;
    if (state.mode === 'invalid-then-valid') {
      const invalid = Buffer.from(packet);
      invalid[1] ^= 1;
      socket.send(invalid, remote.port, remote.address);
    }
    socket.send(packet, remote.port, remote.address);
  });
  await new Promise((resolve, reject) => {
    socket.once('error', reject);
    socket.bind(port, host, resolve);
  });
  state.port = socket.address().port;
  state.reset = mode => { state.mode = mode; state.count = 0; state.validPings = 0; };
  return state;
}

try {
  const server = await fakeServer();
  browser = spawn(binary, [
    '--headless=new', '--no-first-run', '--no-default-browser-check',
    '--disable-background-networking', '--remote-debugging-port=0',
    '--host-resolver-rules=MAP mcbe-test.invalid ~NOTFOUND',
    `--user-data-dir=${profile}`, '--enable-logging=stderr',
    ...chromeArgs, 'about:blank',
  ], {stdio: ['ignore', log, log], windowsHide: true});
  let launchError;
  browser.on('error', error => { launchError = error; });
  const portFile = path.join(profile, 'DevToolsActivePort');
  const port = await waitFor(() => {
    if (launchError) throw launchError;
    if (browser.exitCode !== null) throw new Error(`Chrome exited: ${browser.exitCode}`);
    if (fs.existsSync(portFile)) return fs.readFileSync(portFile, 'utf8').split('\n')[0];
  });
  const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  ws = new WebSocket(targets.find(target => target.type === 'page').webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
  let id = 0;
  const pending = new Map();
  const responses = [];
  ws.onmessage = event => {
    const message = JSON.parse(event.data);
    if (message.id) {
      pending.get(message.id)?.(message);
      pending.delete(message.id);
    } else if (message.method === 'Network.responseReceived') {
      responses.push(message.params.response);
    }
  };
  ws.onclose = () => {
    for (const resolve of pending.values()) {
      resolve({error: {message: 'CDP connection closed'}});
    }
    pending.clear();
  };
  function send(method, params = {}) {
    return new Promise((resolve, reject) => {
      const requestId = ++id;
      const timer = setTimeout(() => {
        pending.delete(requestId);
        reject(new Error(`CDP timed out: ${method}`));
      }, 12000);
      pending.set(requestId, message => {
        clearTimeout(timer);
        message.error ? reject(new Error(`${method}: ${JSON.stringify(message.error)}`)) : resolve(message.result);
      });
      ws.send(JSON.stringify({id: requestId, method, params}));
    });
  }
  async function evaluate(expression) {
    const value = await send('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true});
    if (value.exceptionDetails) throw new Error(JSON.stringify(value.exceptionDetails));
    return value.result.value;
  }
  async function pageState() {
    return evaluate(`({url:location.href, origin:location.origin,
      ready:document.readyState,text:document.body?.innerText ?? '',
      state:document.querySelector('#query-state')?.textContent,
      injected:!!globalThis.mcbeInjected, images:document.images.length,
      scripts:document.scripts.length, secure:isSecureContext})`);
  }
  async function navigate(url, expectedUrl = url) {
    const navigation = await send('Page.navigate', {url});
    if (navigation.errorText) throw new Error(`Navigation failed: ${navigation.errorText}`);
    let state;
    try {
      return await waitFor(async () => {
        state = await pageState();
        return state.url === expectedUrl && state.ready === 'complete' && state.state && state;
      }, 9000);
    } catch (error) {
      throw new Error(`Navigation to ${url}: ${error.message}; last page: ${JSON.stringify(state)}`);
    }
  }
  await send('Page.enable');
  await send('Network.enable');
  const url = `mcbe://127.0.0.1:${server.port}/`;
  await check('native navigation, UTF-8, escaped HTML and GUID precision', async () => {
    const state = await navigate(url);
    assert.equal(state.state, '查询成功', JSON.stringify(state));
    assert.ok(state.text.includes('测试 <img src=x'));
    assert.ok(state.text.includes('12 / 100'));
    assert.ok(state.text.includes(String(guid)));
    assert.equal(state.images, 0);
    assert.equal(state.scripts, 0);
    assert.equal(state.injected, false);
    assert.equal(state.origin, 'null');
    assert.equal(await evaluate('typeof globalThis.chrome?.send'), 'undefined');
    assert.equal(server.validPings, 1);
    const response = responses.find(response => response.url === url);
    assert.ok(response);
    assert.match(response.headers['Content-Security-Policy'], /default-src 'none'/);
    return state;
  });
  const screenshot = option('screenshot', '');
  async function saveScreenshot() {
    if (!screenshot) return;
    const image = await send('Page.captureScreenshot', {format: 'png', captureBeyondViewport: true});
    fs.writeFileSync(path.resolve(screenshot), Buffer.from(image.data, 'base64'));
  }
  await saveScreenshot();
  await check('reload starts a fresh query', async () => {
    const oldTimeOrigin = await evaluate('performance.timeOrigin');
    await send('Page.reload', {ignoreCache: true});
    await waitFor(() => server.count === 2);
    await waitFor(async () => (await evaluate('performance.timeOrigin')) !== oldTimeOrigin &&
                              (await pageState()).state === '查询成功');
  });
  await check('CSP blocks injected scripts', async () => {
    await evaluate(`{const script=document.createElement('script');
      script.textContent='globalThis.mcbeInjected=1';document.body.append(script);}`);
    assert.equal(await evaluate('!!globalThis.mcbeInjected'), false);
  });
  await check('one lost ping retries once', async () => {
    server.reset('drop-first');
    const state = await navigate(url);
    assert.equal(state.state, '查询成功');
    assert.equal(server.count, 2);
  });
  await check('unrelated pong is ignored before valid response', async () => {
    server.reset('invalid-then-valid');
    assert.equal((await navigate(url)).state, '查询成功');
    assert.equal(server.count, 1);
  });
  await check('missing optional MOTD fields', async () => {
    server.reset('minimal');
    const state = await navigate(url);
    assert.equal(state.state, '查询成功');
    assert.ok(state.text.includes('Minimal'));
  });
  await check('MOTD and RakNet GUIDs may differ', async () => {
    server.reset('different-guid');
    const state = await navigate(url);
    assert.equal(state.state, '查询成功');
    assert.ok(state.text.includes(String(guid)));
    assert.ok(state.text.includes('MOTD GUID\n42'));
  });
  await check('invalid response error and bounded retries', async () => {
    server.reset('invalid');
    assert.equal((await navigate(url)).state, '服务器响应无效');
    assert.equal(server.count, 2);
  });
  await check('silent server times out within total budget', async () => {
    server.reset('silent');
    const start = Date.now();
    assert.equal((await navigate(url)).state, '等待响应超时');
    assert.equal(server.count, 2);
    assert.ok(Date.now() - start < 7500);
  });
  await check('unsupported URL components cause no UDP traffic', async () => {
    server.reset('success');
    for (const invalidUrl of [url + 'path', url + '?query', url + '#fragment',
                              url.replace('mcbe://', 'mcbe://user:password@')]) {
      // Leave the origin first: a fragment-only change is a same-document navigation.
      await send('Page.navigate', {url: 'about:blank'});
      // Chromium strips credentials from the committed document URL.
      const visibleUrl = invalidUrl.replace('user:password@', '');
      assert.equal((await navigate(invalidUrl, visibleUrl)).state, '地址无效');
    }
    assert.equal(server.count, 0);
  });
  await check('navigation cancellation closes the query before retry', async () => {
    server.reset('silent');
    const request = send('Page.navigate', {url}).catch(() => {});
    await waitFor(() => server.count === 1);
    await send('Page.navigate', {url: 'about:blank'});
    await request;
    await delay(3000);
    assert.equal(server.count, 1);
    assert.equal((await pageState()).url, 'about:blank');
  });
  await check('subframe and fetch cannot open the UDP loader', async () => {
    server.reset('success');
    await evaluate(`{const frame=document.createElement('iframe');
      frame.src=${JSON.stringify(url)};document.body.append(frame);}`);
    assert.equal(await evaluate(`fetch(${JSON.stringify(url)}).then(()=>false,()=>true)`), true);
    await delay(500);
    assert.equal(server.count, 0);
  });
  await check('localhost DNS and address fallback', async () => {
    server.reset('success');
    assert.equal((await navigate(`mcbe://localhost:${server.port}/`)).state, '查询成功');
  });
  await check('DNS failure renders a local error page', async () => {
    const state = await navigate('mcbe://mcbe-test.invalid/');
    assert.equal(state.state, 'DNS 解析失败');
  });
  await check('explicit IPv6 endpoint', async () => {
    const ipv6 = await fakeServer('udp6', '::1');
    assert.equal((await navigate(`mcbe://[::1]:${ipv6.port}/`)).state, '查询成功');
    assert.equal(ipv6.validPings, 1);
  });
  await check('back and forward preserve native URL and result', async () => {
    server.reset('success');
    await navigate(url);
    await send('Page.navigate', {url: 'about:blank'});
    await evaluate('setTimeout(() => history.back(), 50)');
    await waitFor(async () => (await pageState()).url === url &&
                              (await pageState()).state === '查询成功');
    await evaluate('setTimeout(() => history.forward(), 50)');
    await waitFor(async () => (await pageState()).url === 'about:blank');
  });
  const publicHost = option('public-host', '');
  if (publicHost) {
    await check('public Bedrock server', async () => {
      const state = await navigate(`mcbe://${publicHost}/`);
      assert.equal(state.state, '查询成功', state.text);
      await saveScreenshot();
      return state;
    });
  }
  await check('browser shutdown cancels an outstanding query', async () => {
    server.reset('silent');
    const navigation = send('Page.navigate', {url}).catch(() => {});
    await waitFor(() => server.count === 1);
    const browserPath = fs.readFileSync(portFile, 'utf8').split('\n')[1].trim();
    const control = new WebSocket(`ws://127.0.0.1:${port}${browserPath}`);
    await new Promise((resolve, reject) => {
      control.onopen = resolve;
      control.onerror = reject;
    });
    control.send(JSON.stringify({id: 1, method: 'Browser.close'}));
    await waitFor(() => browser.exitCode !== null, 8000);
    control.close();
    await navigation;
    await delay(3000);
    assert.equal(server.count, 1);
    assert.equal(browser.exitCode, 0);
  });
} catch (error) {
  console.error(error.stack ?? error);
  if (!results.some(result => !result.passed)) {
    results.push({name: 'test harness', passed: false, error: String(error.stack ?? error)});
  }
  process.exitCode = 1;
} finally {
  ws?.close();
  for (const socket of sockets) {
    try { socket.close(); } catch {}
  }
  if (browser?.pid && browser.exitCode === null) {
    // Only terminate the browser process tree created by this test.
    if (process.platform === 'win32') {
      spawnSync('taskkill', ['/PID', String(browser.pid), '/T', '/F'], {windowsHide: true, stdio: 'ignore'});
    } else {
      browser.kill('SIGKILL');
    }
    await delay(200);
  }
  fs.closeSync(log);
  fs.writeFileSync(output, JSON.stringify({binary, chromeArgs, results}, null, 2));
  assert.equal(path.dirname(path.resolve(profile)), temporaryRoot);
  assert.ok(path.basename(profile).startsWith('mcbe-smoke-'));
  fs.rmSync(profile, {recursive: true, force: true, maxRetries: 4, retryDelay: 200});
  console.log(`Results: ${output}`);
}
