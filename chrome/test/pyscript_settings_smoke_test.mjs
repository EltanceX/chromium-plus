// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Real WebUI form, persisted Local State and sandboxed Renderer regression.
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import {pathToFileURL} from 'node:url';

const option = (name, fallback) => process.argv.slice(2)
    .find(arg => arg.startsWith(`--${name}=`))?.slice(name.length + 3) ?? fallback;
const binary = path.resolve(option('chrome-binary', 'out/Debug/chrome.exe'));
const output = path.resolve(option('output', 'out/Debug/pyscript-settings-smoke.json'));
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'pyscript-settings-'));
const profile = path.join(root, 'profile');
fs.mkdirSync(path.dirname(output), {recursive: true});
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const block = source => `<pyscript>${source}</pyscript>`;
const html = '<!doctype html><meta charset="utf-8"><p id="result">before</p>' +
    block(`assert 'page_marker' not in globals()\npage_marker = True\n` +
        `import dom\ndom.set_text(dom.get_element_by_id('result'), 'Python settings 42')`);
const file = path.join(root, 'python.html');
fs.writeFileSync(file, html);
const server = http.createServer((request, response) => {
  const policy = request.url === '/blocked' ?
      {'Content-Security-Policy': "script-src 'none'; sandbox"} : {};
  response.writeHead(200, {'Content-Type': 'text/html; charset=utf-8', ...policy});
  if (request.url === '/parent') {
    response.end('<!doctype html><iframe src="/result"></iframe>' +
        `<iframe srcdoc="${html.replaceAll('&', '&amp;').replaceAll('"', '&quot;')}"></iframe>`);
  } else if (request.url === '/loop') {
    response.end('<!doctype html><p id="result">before</p>' +
        block('while True:\n    pass') + block('raise AssertionError("FORBIDDEN_LATER")'));
  } else {
    response.end(html);
  }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
const chromeArgs = ['--enable-features=NativePythonRuntimeSelfTest'];
const result = {binary, sandboxDisabled: false, chromeArgs, cases: []};
let browser;
let launchNumber = 0;

async function waitFor(predicate, timeout = 30000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    const value = await predicate();
    if (value) return value;
    await delay(50);
  }
  throw new Error(`Timed out after ${timeout} ms`);
}

async function launch() {
  const logPath = `${output}.launch-${++launchNumber}.chrome.log`;
  const log = fs.openSync(logPath, 'w');
  const portFile = path.join(profile, 'DevToolsActivePort');
  if (fs.existsSync(portFile)) fs.unlinkSync(portFile);
  const child = spawn(binary, ['--headless', '--disable-gpu', '--no-first-run',
    '--no-default-browser-check', '--disable-background-networking', '--no-proxy-server',
    '--remote-debugging-port=0', '--enable-logging=stderr',
    `--user-data-dir=${profile}`, ...chromeArgs, 'about:blank'],
    {windowsHide: true, stdio: ['ignore', log, log]});
  let launchError;
  child.once('error', error => { launchError = error; });
  let ws;
  let id = 0;
  const pending = new Map();
  const events = [];
  async function close() {
    try {
      if (ws?.readyState === WebSocket.OPEN && child.exitCode === null) {
        await send('Browser.close');
        await waitFor(() => child.exitCode !== null, 15000);
      }
    } finally {
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
  }
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
  try {
    const port = await waitFor(() => {
      if (launchError) throw launchError;
      if (child.exitCode !== null) throw new Error(`Chrome exited: ${child.exitCode}`);
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
    const evaluate = async expression => {
      const data = await send('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true});
      assert.ok(!data.exceptionDetails, JSON.stringify(data.exceptionDetails));
      return data.result.value;
    };
    await send('Page.enable');
    await send('Runtime.enable');
    await send('Log.enable');
    return {send, evaluate, close, events, logPath, async navigate(url) {
      events.length = 0;
      const navigation = await send('Page.navigate', {url});
      assert.ok(!navigation.errorText, navigation.errorText);
      await waitFor(() => events.some(event => event.method === 'Page.loadEventFired'));
    }};
  } catch (error) {
    await close();
    throw error;
  }
}

const findPage = `(() => {
  function find(root) {
    const direct = root.querySelector('settings-kernel-debug-page');
    if (direct) return direct;
    for (const node of root.querySelectorAll('*')) {
      if (node.shadowRoot) { const found = find(node.shadowRoot); if (found) return found; }
    }
    return null;
  }
  return find(document);
})()`;
async function settingsPage() {
  await browser.navigate('chrome://settings/kernelDebug');
  await waitFor(() => browser.evaluate(`!!${findPage}?.pythonSettings_`));
}
async function readSettings() {
  return browser.evaluate(`${findPage}.pythonSettings_`);
}
async function saveForm(enabled, unrestricted, allowedOrigin) {
  await browser.evaluate(`(() => {
    const root = ${findPage}.shadowRoot;
    const enabled = root.querySelector('#pythonEnabled');
    if (enabled.checked !== ${enabled}) enabled.click();
  })()`);
  await delay(30);
  // A disabled interpreter retains the saved permission choice, so set that
  // control before disabling in callers that need to change both values.
  if (enabled) {
    await browser.evaluate(`(() => {
      const root = ${findPage}.shadowRoot;
      const toggle = root.querySelector('#pythonUnrestricted');
      if (toggle.checked !== ${unrestricted}) toggle.click();
    })()`);
    await delay(30);
  }
  await browser.evaluate(`(() => {
    const root = ${findPage}.shadowRoot;
    const input = root.querySelector('#pythonAllowedOrigin');
    input.value = ${JSON.stringify(allowedOrigin)};
    input.dispatchEvent(new Event('input', {bubbles:true, composed:true}));
    root.querySelector('#savePythonSettings').click();
  })()`);
  await waitFor(() => browser.evaluate(`${findPage}.pythonMessage_ !== '' && !${findPage}.pythonSaving_`));
  return browser.evaluate(`${findPage}.shadowRoot.querySelector('#pythonMessage').textContent.trim()`);
}
async function text() {
  return browser.evaluate('document.querySelector("#result")?.textContent');
}
async function check(name, callback) {
  const start = Date.now();
  try {
    const detail = await callback();
    result.cases.push({name, passed:true, elapsedMs:Date.now()-start, detail});
    console.log(`PASS ${name}`);
  } catch (error) {
    result.cases.push({name, passed:false, error:String(error.stack ?? error)});
    throw error;
  }
}

try {
  browser = await launch();
  await check('settings page defaults and invalid origin validation', async () => {
    await settingsPage();
    const budgetText = await browser.evaluate(`${findPage}.shadowRoot.textContent`);
    assert.match(budgetText, /初始化预算：单块约 100 ms/);
    assert.match(budgetText, /每回调约 50 ms、输出 4 KiB、DOM 调用 128 次/);
    assert.match(budgetText, /动画帧预算：每帧约 8 ms/);
    assert.match(budgetText, /最近 1 秒帧执行累计约 200 ms/);
    assert.match(budgetText, /点击、键盘、指针等事件与动画帧共享/);
    assert.match(budgetText, /保留一个待执行帧至后续刷新，不补发旧帧/);
    assert.match(budgetText, /最近 1 秒累计约 250 ms/);
    assert.match(budgetText, /清理另有约 50 ms/);
    const defaults = await readSettings();
    assert.equal(defaults.enabled, false);
    assert.equal(defaults.activeEnabled, false);
    assert.equal(defaults.supported, true);
    assert.match(await saveForm(true, false, 'https://example.com'), /本地 HTTP/);
    assert.equal((await readSettings()).enabled, false);
    assert.match(await saveForm(true, false, origin), /已保存/);
    const saved = await readSettings();
    assert.equal(saved.restartRequired, true);
    assert.equal(saved.activeEnabled, false);
    await browser.navigate(origin + '/result');
    assert.equal(await text(), 'before');
    return {defaults, saved};
  });
  await browser.close(); browser = await launch();
  await check('restricted settings survive browser restart and enable Python', async () => {
    await settingsPage();
    const active = await readSettings();
    assert.equal(active.activeEnabled, true);
    assert.equal(active.activeUnrestricted, false);
    assert.equal(active.restartRequired, false);
    await browser.navigate(origin + '/result');
    assert.equal(await text(), 'Python settings 42');
    return active;
  });
  await check('restricted mode still enforces CSP and exact origin', async () => {
    await browser.navigate(origin + '/blocked');
    assert.equal(await text(), 'before');
    await browser.navigate(origin.replace('127.0.0.1', 'localhost') + '/result');
    assert.equal(await text(), 'before');
  });
  await check('unrestricted choice is saved without changing active renderers', async () => {
    await settingsPage();
    assert.match(await saveForm(true, true, origin), /已保存/);
    const state = await readSettings();
    assert.equal(state.restartRequired, true);
    assert.equal(state.activeUnrestricted, false);
    await browser.navigate(origin + '/blocked');
    assert.equal(await text(), 'before');
    return state;
  });
  await browser.close(); browser = await launch();
  await check('unrestricted mode bypasses CSP sandbox and origin gates after restart', async () => {
    await settingsPage();
    const active = await readSettings();
    assert.equal(active.activeUnrestricted, true);
    await browser.navigate(origin + '/blocked');
    assert.equal(await browser.evaluate('self.origin'), 'null');
    assert.equal(await text(), 'Python settings 42');
    await browser.navigate(origin.replace('127.0.0.1', 'localhost') + '/result');
    assert.equal(await text(), 'Python settings 42');
    return active;
  });
  await check('file and opaque data HTML can execute with the explicit override', async () => {
    await browser.navigate(pathToFileURL(file).href);
    assert.equal(await text(), 'Python settings 42');
    await browser.navigate('data:text/html;charset=utf-8,' + encodeURIComponent(html));
    assert.equal(await text(), 'Python settings 42');
  });
  await check('ordinary iframe and srcdoc have independent Python contexts', async () => {
    await browser.navigate(origin + '/parent');
    const values = await browser.evaluate(`Array.from(document.querySelectorAll('iframe'),
      frame => frame.contentDocument.querySelector('#result').textContent)`);
    assert.deepEqual(values, ['Python settings 42', 'Python settings 42']);
    return values;
  });
  await check('page JavaScript setting does not block the Python override', async () => {
    await browser.send('Emulation.setScriptExecutionDisabled', {value:true});
    try {
      await browser.navigate(origin + '/result');
      assert.equal(await text(), 'Python settings 42');
    } finally {
      await browser.send('Emulation.setScriptExecutionDisabled', {value:false});
    }
  });
  await check('execution budgets still stop later blocks in unrestricted mode', async () => {
    await browser.navigate(origin + '/loop');
    const logs = browser.events.filter(e => e.method === 'Log.entryAdded').map(e => e.params.entry.text);
    assert.ok(logs.some(message => message.includes('100 ms cooperative')));
    assert.ok(!logs.some(message => message.includes('FORBIDDEN_LATER')));
    assert.equal(await text(), 'before');
    return logs;
  });
  await check('unrestricted Python renderers retain restricted sandbox tokens', async () => {
    const log = fs.readFileSync(browser.logPath, 'utf8');
    const reports = [...log.matchAll(/CPYTHON_RUNTIME_SELF_TEST (\{[^\r\n]+\})/g)]
        .map(match => JSON.parse(match[1]));
    assert.ok(reports.length > 0);
    assert.ok(reports.every(report => report.passed && report.sandboxed));
    return reports;
  });
  await check('settings form reports active mode and saved disable requires restart', async () => {
    await settingsPage();
    const screenshot = await browser.send('Page.captureScreenshot', {format:'png'});
    fs.writeFileSync(`${output}.settings.png`, Buffer.from(screenshot.data, 'base64'));
    assert.match(await saveForm(false, true, origin), /已保存/);
    const saved = await readSettings();
    assert.equal(saved.enabled, false);
    assert.equal(saved.activeEnabled, true);
    assert.equal(saved.restartRequired, true);
    return saved;
  });
  await browser.close(); browser = await launch();
  await check('disabling persists and does not leak enabled switches on relaunch', async () => {
    await settingsPage();
    const active = await readSettings();
    assert.equal(active.activeEnabled, false);
    assert.equal(active.activeUnrestricted, false);
    await browser.navigate(origin + '/result');
    assert.equal(await text(), 'before');
    return active;
  });
  result.passed = true;
} catch (error) {
  result.passed = false;
  result.error = String(error.stack ?? error);
  process.exitCode = 1;
} finally {
  await browser?.close().catch(error => {result.closeError = String(error); process.exitCode = 1;});
  await new Promise(resolve => server.close(resolve));
  fs.writeFileSync(output, JSON.stringify(result, null, 2)+'\n');
  assert.equal(path.dirname(path.resolve(root)), path.resolve(os.tmpdir()));
  assert.ok(path.basename(root).startsWith('pyscript-settings-'));
  fs.rmSync(root, {recursive:true, force:true, maxRetries:8, retryDelay:250});
}
console.log(JSON.stringify({passed:result.passed, cases:result.cases.length, output, error:result.error}, null, 2));
