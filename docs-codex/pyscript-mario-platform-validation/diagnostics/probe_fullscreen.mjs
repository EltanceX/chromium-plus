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
const output = path.resolve(option('output', 'out/Debug/pyscript-browser-smoke.json'));
const chromeArgs = process.argv.slice(2).filter(arg => arg.startsWith('--chrome-arg='))
    .map(arg => arg.slice('--chrome-arg='.length));
assert.ok(!chromeArgs.some(arg => /no-sandbox|disable-web-security/.test(arg)));
fs.mkdirSync(path.dirname(output), {recursive: true});
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'pyscript-browser-'));
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));

const sample = fs.readFileSync('chrome/test/data/pyscript/mario.html', 'utf8');
const downloads = path.join(root, 'downloads');
fs.mkdirSync(downloads);
let exported;
const wrap = source => `<!doctype html><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="script-src 'nonce-phase6'; object-src 'none'">
<style>button{margin:8px;padding:12px}#stage{background:#def}#stage:fullscreen{background:#def}</style>
<div id="stage"><p id="result">initializing</p><p id="completion"></p></div>
<button id="audio">Audio</button><button id="suspend">Suspend</button><button id="limit">Limit</button>
<button id="fullscreen">Fullscreen</button><button id="export">Export</button><button id="drain">Drain</button>
<pyscript nonce="phase6">import dom, browser, animation
target = dom.get_element_by_id('result')
completed = dom.get_element_by_id('completion')
def expect(error, fn):
    try: fn()
    except error: return
    raise AssertionError('expected '+error.__name__)
${source}</pyscript>`;
const api = wrap(`
import time
assert browser.storage_get('missing-phase6') is None
browser.storage_set('phase6Value', '中😀')
assert browser.storage_get('phase6Value') == '中😀'
expect(PermissionError, browser.audio_resume)
expect(PermissionError, lambda: browser.export_document('game.html'))
expect(ValueError, lambda: browser.export_document('../game.html'))
pending = {}
limit_stage, limit_started = 0, 0
def request(e):
    pending[browser.audio_resume()] = 'audio'
def suspend(e):
    pending[browser.audio_suspend()] = 'suspend'
def fullscreen(e):
    token = browser.exit_fullscreen() if browser.is_fullscreen() else browser.request_fullscreen(dom.get_element_by_id('stage'))
    pending[token] = 'fullscreen'
def export(e):
    browser.export_document('phase6-api.html')
    dom.set_text(target, 'export-submitted')
def limit(e):
    global limit_stage, limit_started
    if limit_stage == 0: limit_started = time.perf_counter()
    browser.audio_stop()
    tones = [('triangle', 220, 440, 2, 0, .01)]*16
    browser.play_tones(tones)
    if limit_stage == 0:
        expect(ValueError, lambda: browser.play_tones(tones[:1]))
    browser.audio_stop()
    limit_stage += 1
    assert time.perf_counter()-limit_started < 1
    if limit_stage == 4:
        expect(ValueError, lambda: browser.play_tones(tones[:1]))
        dom.set_text(target, 'voices-and-rate-bounded')
    else: dom.set_text(target, 'limit-stage:'+str(limit_stage))
def pump(t):
    item = browser.poll()
    if item:
        id, ok, detail = item
        action = pending.pop(id, 'unknown')
        if action == 'audio' and ok:
            browser.play_tones([('square', 220, 620, .15, 0, .03), ('triangle', 990, 990, .18, .07, .03)])
        dom.set_text(completed, action+':'+str(ok)+':'+browser.audio_state()+':'+str(browser.is_fullscreen()))
    animation.request_frame(pump)
for name, fn in (('audio',request),('suspend',suspend),('fullscreen',fullscreen),('export',export),('limit',limit)):
    dom.add_event_listener(dom.get_element_by_id(name), 'click', fn)
animation.request_frame(pump)
dom.set_text(target, 'api-ready')
`);
const pendingPage = wrap(`
ids = [browser.request_fullscreen(dom.get_element_by_id('stage')) for _ in range(8)]
assert len(set(ids)) == 8
expect(ValueError, lambda: browser.request_fullscreen(dom.get_element_by_id('stage')))
received = []
def drain(e):
    item = browser.poll()
    if item:
        assert item[0] in ids and item[0] not in received and not item[1]
        received.append(item[0])
    else: assert len(received) == 8
    dom.set_text(target, 'drained:'+str(len(received)))
dom.add_event_listener(dom.get_element_by_id('drain'), 'click', drain)
dom.set_text(target, 'bounded-pending')
`);
const fatalAudio = wrap(`
pending = None
def start(e):
    global pending
    pending = browser.audio_resume()
def pump(t):
    if pending:
        result = browser.poll()
        if result:
            assert result[1]
            browser.play_tones([('sine', 440, 440, 2, 4, .01)])
            dom.set_text(target, 'fatal-started')
            while True: pass
    animation.request_frame(pump)
dom.add_event_listener(dom.get_element_by_id('audio'), 'click', start)
animation.request_frame(pump)
dom.set_text(target, 'fatal-ready')
`);
const win = sample.replace('<!-- initUI() -->', `<!-- explicit terminal-score fixture -->
<pyscript nonce="python-mario">game.score = 1200
game.remaining = 100
winGame()</pyscript>
<!-- initUI() -->`);
const pages = new Map([
  ['/api', api], ['/pending', pendingPage], ['/fatal', fatalAudio], ['/sample', sample], ['/win', win],
  ['/read', wrap("dom.set_text(target, str(browser.storage_get('phase6Value')))")],
  ['/denied', wrap("expect(PermissionError, lambda: browser.storage_get('phase6Value'))\ndom.set_text(target, 'storage-denied')")],
  ['/quota', wrap(`def write(e):
    expect(PermissionError, lambda: browser.storage_set('quota-check', 'x'*16384))
    dom.set_text(target, 'quota-denied')
dom.add_event_listener(dom.get_element_by_id('audio'), 'click', write)
dom.set_text(target, 'quota-ready')`)],
  ['/other', '<!doctype html><p id="result">other</p>'],
]);
const server = http.createServer((req, res) => {
  const body = req.url === '/exported' ? exported : pages.get(req.url);
  if (!body) return res.writeHead(404).end();
  res.writeHead(200, {'Content-Type':'text/html;charset=utf-8'}).end(body);
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
const report = {binary, origin, sandboxDisabled:false, chromeArgs, cases:[]};
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
let launches = 0;
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
  fs.mkdirSync(profile, {recursive:true});
  log = fs.openSync(`${output}.chrome.log`, launches++ ? 'a' : 'w');
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
  for (const domain of ['Page', 'Runtime', 'Log', 'WebAudio']) await send(`${domain}.enable`);
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
    const failure = await send('Page.captureScreenshot',{format:'png'}).catch(() => null);
    if (failure) fs.writeFileSync(`${output}.failure.png`,Buffer.from(failure.data,'base64'));
    report.cases.push({name, passed: false, elapsedMs: Date.now() - start,
      error: String(error.stack ?? error), nativeLogs: nativeLogs(), pageState: await evaluate("({result:document.getElementById('result')?.textContent,completion:document.getElementById('completion')?.textContent,status:document.getElementById('status')?.textContent,telemetry:document.getElementById('telemetry')?.textContent,metrics:document.getElementById('metrics')?.textContent})").catch(() => null)});
    throw error;
  }
}
const errors = () => nativeLogs().filter(entry => entry.level === 'error');
const read = id => evaluate(`document.getElementById(${JSON.stringify(id)}).textContent`);
async function key(type, code, keyValue, virtualKey, extra = {}) {
  await send('Input.dispatchKeyEvent', {type, code, key: keyValue,
    windowsVirtualKeyCode: virtualKey, nativeVirtualKeyCode: virtualKey, ...extra});
}
async function focusByTab(id) {
  for (let n=0;n<30;n++) {
    if (await evaluate('document.activeElement.id') === id) return;
    await key('keyDown','Tab','Tab',9); await key('keyUp','Tab','Tab',9);
  }
  assert.fail('Native Tab did not reach '+id);
}

let lastTrustedClick = 0;
async function trustedClick(id) {
  // Use a human input cadence. The unchanged shared window deliberately drops
  // events under pressure; functional UI checks must not send 30 clicks/second.
  await delay(Math.max(0,120-(Date.now()-lastTrustedClick)));
  await evaluate(`document.getElementById(${JSON.stringify(id)}).scrollIntoView({block:'center',behavior:'instant'})`);
  await evaluate(`globalThis.__nativeClick=null;document.getElementById(${JSON.stringify(id)}).addEventListener('click',e=>globalThis.__nativeClick={id:e.currentTarget.id,trusted:e.isTrusted},{once:true})`);
  const rect = await evaluate(`(() => {const r=document.getElementById(${JSON.stringify(id)}).getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()`);
  await send('Input.dispatchMouseEvent',{...rect,type:'mouseMoved',buttons:0});
  for (const type of ['mousePressed', 'mouseReleased']) await send('Input.dispatchMouseEvent',
      {...rect, type, button: 'left', buttons:type === 'mousePressed' ? 1 : 0, clickCount: 1});
  lastTrustedClick = Date.now();
  assert.deepEqual(await evaluate('globalThis.__nativeClick'),{id,trusted:true});
}

try {
  await launch();
  for (const front of [false,true]) {
    if(front) await send('Page.bringToFront');
    await navigate('/api');
    await evaluate(`(()=>{let b=document.getElementById('fullscreen');let n=b.cloneNode(true);b.replaceWith(n);n.addEventListener('click',()=>document.getElementById('stage').requestFullscreen().then(()=>document.getElementById('completion').textContent='native-success').catch(e=>document.getElementById('completion').textContent=e.name+':'+e.message));})()`);
    const before=await evaluate('({focus:document.hasFocus(),active:navigator.userActivation.isActive})');
    await trustedClick('fullscreen'); await delay(1500);
    console.log(JSON.stringify({front,before,after:await evaluate("({focus:document.hasFocus(),active:navigator.userActivation.isActive,fullscreen:document.fullscreenElement?.id,result:document.getElementById('completion').textContent})")}));
  }
} finally {
  await close(); await new Promise(resolve=>server.close(resolve));
  fs.rmSync(root,{recursive:true,force:true,maxRetries:8,retryDelay:250});
}
