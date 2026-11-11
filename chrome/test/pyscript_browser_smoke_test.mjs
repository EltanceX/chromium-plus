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
browser.storage_set('phase6Empty', '')
assert browser.storage_get('phase6Empty') == ''
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
  const portFile = path.join(profile, 'DevToolsActivePort');
  if (fs.existsSync(portFile)) fs.unlinkSync(portFile);
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
      error: String(error.stack ?? error), nativeLogs: nativeLogs(),
      downloadEvents: events.filter(e=>e.method.startsWith('Browser.download')),
      pageState: await evaluate("({result:document.getElementById('result')?.textContent,completion:document.getElementById('completion')?.textContent,status:document.getElementById('status')?.textContent,telemetry:document.getElementById('telemetry')?.textContent,metrics:document.getElementById('metrics')?.textContent,toast:document.getElementById('toast')?.textContent})").catch(() => null)});
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
  await send('Page.bringToFront');
  assert.equal(await evaluate('document.hasFocus()'),true);
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
  await send('Browser.setDownloadBehavior', {behavior:'allow',downloadPath:downloads,eventsEnabled:true});
  await check('Typed storage and real activation gating', async () => {
    await navigate('/api');
    await waitFor(async () => await text() === 'api-ready');
    assert.deepEqual(errors(), []);
    assert.equal(events.filter(e => e.method === 'WebAudio.contextCreated').length,0);
  });
  await check('Synthetic click cannot unlock audio or export', async () => {
    pages.set('/synthetic', wrap(`def click(e):
    expect(PermissionError, browser.audio_resume)
    expect(PermissionError, lambda: browser.export_document('game.html'))
    dom.set_text(target, 'denied-without-activation')
dom.add_event_listener(dom.get_element_by_id('audio'), 'click', click)`));
    await navigate('/synthetic'); await click('audio');
    assert.equal(await text(),'denied-without-activation');
    assert.deepEqual(errors(), []);
  });
  await check('Native audio resumes and schedules oscillator and gain nodes', async () => {
    await navigate('/api'); await trustedClick('audio');
    await waitFor(async () => (await read('completion')).startsWith('audio:True:running'));
    const contexts=events.filter(e=>e.method==='WebAudio.contextCreated');
    const nodes=events.filter(e=>e.method==='WebAudio.audioNodeCreated');
    assert.equal(contexts.length,1);
    // The DevTools agent strips the Node suffix from WebAudio node names.
    assert.ok(nodes.some(e=>e.params.node.nodeType==='Oscillator'));
    assert.ok(nodes.some(e=>e.params.node.nodeType==='Gain'));
    return {contexts:contexts.map(e=>e.params.context),nodes:nodes.map(e=>e.params.node.nodeType)};
  });
  await check('Native audio suspend completes once', async () => {
    await trustedClick('suspend');
    await waitFor(async () => (await read('completion')).startsWith('suspend:True:suspended'));
    const completion=await read('completion'); await delay(150);
    assert.equal(await read('completion'),completion);
  });
  await check('Voice and scheduling rate limits reject additional sound', async () => {
    await trustedClick('audio');
    await waitFor(async () => (await read('completion')).startsWith('audio:True:running'));
    await delay(1100);
    for (let stage=1;stage<=4;stage++) {
      await trustedClick('limit');
      await waitFor(async()=>await text()===(stage===4 ? 'voices-and-rate-bounded' : 'limit-stage:'+stage));
    }
    await waitFor(async () => await text()==='voices-and-rate-bounded');
    assert.deepEqual(errors(), []);
  });
  await check('Navigation disposes document audio resources', async () => {
    await navigate('/other'); await delay(100);
    assert.equal(await text(),'other');
    await navigate('/api'); await waitFor(async()=>await text()==='api-ready');
    assert.equal(await evaluate('document.fullscreenElement'),null);
    assert.deepEqual(errors(), []);
  });
  await check('Fullscreen rejection caps and consumes notifications exactly once', async () => {
    await navigate('/pending'); assert.equal(await text(),'bounded-pending');
    await delay(150);
    for(let i=1;i<=8;i++) { await click('drain'); await delay(120); assert.equal(await text(),'drained:'+i); }
    await click('drain'); assert.equal(await text(),'drained:8');
    assert.equal(await evaluate('document.fullscreenElement'),null);
    assert.deepEqual(errors(), []);
  });
  await check('Trusted fullscreen request and native exit complete', async () => {
    await navigate('/api'); await trustedClick('fullscreen');
    await waitFor(async()=>await evaluate('document.fullscreenElement?.id')==='stage');
    await waitFor(async()=>(await read('completion')).startsWith('fullscreen:True:'));
    // The test driver can click an existing native exit button after moving it
    // into the top layer; no Python source or evaluated JS supplies activation.
    await evaluate('document.getElementById("stage").appendChild(document.getElementById("fullscreen"))');
    await trustedClick('fullscreen');
    await waitFor(async()=>await evaluate('document.fullscreenElement')===null);
    await waitFor(async()=>(await read('completion')).startsWith('fullscreen:True:') &&
        (await read('completion')).endsWith(':False'));
    assert.deepEqual(errors(), []);
  });
  await check('Storage quota failure reaches Python without stopping page', async () => {
    await navigate('/quota');
    const quotaError = await evaluate(`(()=>{localStorage.clear();let i=0;try{for(;i<1000;i++)localStorage.setItem('quota-'+i,'x'.repeat(16384))}catch(e){return e.name}})()`);
    assert.equal(quotaError,'QuotaExceededError');
    await trustedClick('audio'); assert.equal(await text(),'quota-denied');
    await evaluate('localStorage.clear()'); assert.deepEqual(errors(), []);
  });
  await check('Terminal score fixture persists highest score and Mario toggles sound', async () => {
    await navigate('/win');
    await waitFor(async()=>await evaluate('localStorage.getItem("marioLabBest")')==='2200');
    await navigate('/sample'); await waitFor(async()=>await read('bestCount')==='2200');
    await trustedClick('soundButton');
    await waitFor(async()=>await evaluate('document.getElementById("soundButton").getAttribute("aria-pressed")')==='true');
    await delay(500); await trustedClick('soundButton');
    await waitFor(async()=>await evaluate('document.getElementById("soundButton").getAttribute("aria-pressed")')==='false');
    await delay(500); await focusByTab('game');
    await key('keyDown','KeyM','m',77); await key('keyUp','KeyM','m',77);
    await waitFor(async()=>await evaluate('document.getElementById("soundButton").getAttribute("aria-pressed")')==='true');
    await delay(500);
    await key('keyDown','KeyM','m',77); await key('keyUp','KeyM','m',77);
    await waitFor(async()=>await evaluate('document.getElementById("soundButton").getAttribute("aria-pressed")')==='false');
    assert.deepEqual(errors(), []);
    const shot=await send('Page.captureScreenshot',{format:'png'});
    fs.writeFileSync(`${output}.platform.png`,Buffer.from(shot.data,'base64'));
  });
  await check('Download preserves Python nonce and runs as a self-contained page', async () => {
    await trustedClick('downloadButton');
    await waitFor(async()=>(await read('toast')).includes('源码'));
    assert.match(await read('toast'),/源码已提交下载/);
    await waitFor(()=>fs.existsSync(path.join(downloads,'python-mario-lab.html')));
    exported=fs.readFileSync(path.join(downloads,'python-mario-lab.html'),'utf8');
    assert.ok(Buffer.byteLength(exported)<1024*1024);
    assert.equal((exported.match(/<pyscript\b/g)||[]).length,12);
    assert.equal((exported.match(/<pyscript nonce="python-mario"/g)||[]).length,12);
    assert.ok(!/<script\b/i.test(exported));
    assert.match(exported,/import browser/);
    await navigate('/exported');
    assert.equal(await evaluate(`Array.from(document.querySelectorAll('*')).flatMap(e=>Array.from(e.attributes)).filter(a=>/^on/i.test(a.name)).length`),0);
    await waitFor(async()=>await read('bestCount')==='2200' && (await read('telemetry')).includes('state:ready'));
    assert.equal(await evaluate('document.querySelectorAll(".lesson-map button").length'),8);
    assert.equal(await evaluate('document.getElementById("soundButton").getAttribute("aria-pressed")'),'false');
    await trustedClick('start'); await waitFor(async()=>(await read('telemetry')).includes('state:playing'));
    assert.deepEqual(errors(), []);
    return {bytes:Buffer.byteLength(exported),blocks:12,noncePreserved:true,reloaded:true};
  });
  await check('Stored score survives normal browser process restart', async () => {
    await send('Browser.close'); await waitFor(()=>child.exitCode!==null); assert.equal(child.exitCode,0);
    await close(); events.length=0; await launch();
    await navigate('/sample'); await waitFor(async()=>await read('bestCount')==='2200');
    assert.deepEqual(errors(), []);
  });
  await check('Fatal Python stop closes scheduled audio and frame work', async () => {
    await navigate('/fatal'); await trustedClick('audio');
    await waitFor(()=>errors().some(e=>/exceeded/.test(e.text)));
    await delay(200); assert.equal(await text(),'fatal-started');
    assert.ok(events.some(e=>e.method==='WebAudio.contextWillBeDestroyed' ||
      (e.method==='WebAudio.contextChanged' && e.params.context.contextState==='closed')));
    const count=events.filter(e=>e.method==='WebAudio.audioNodeCreated').length;
    await delay(250); assert.equal(events.filter(e=>e.method==='WebAudio.audioNodeCreated').length,count);
    return {nativeErrors:errors()};
  });
  await check('Storage content setting denial leaves Mario playable', async () => {
    await send('Browser.close'); await waitFor(()=>child.exitCode!==null); await close();
    const prefsPath=path.join(root,'restricted','Default','Preferences');
    const prefs=JSON.parse(fs.readFileSync(prefsPath));
    prefs.profile ??= {}; prefs.profile.default_content_setting_values ??= {};
    prefs.profile.default_content_setting_values.cookies=2;
    fs.writeFileSync(prefsPath,JSON.stringify(prefs));
    events.length=0; await launch();
    await navigate('/denied'); assert.equal(await text(),'storage-denied');
    await navigate('/sample'); await waitFor(async()=>(await read('telemetry')).includes('state:ready'));
    await trustedClick('start'); await waitFor(async()=>(await read('telemetry')).includes('state:playing'));
    assert.deepEqual(errors(), []);
  });
  await check('Browser closes with native platform and frame resources', async () => {
    await send('Browser.close'); await waitFor(()=>child.exitCode!==null); assert.equal(child.exitCode,0);
  });
  report.passed=true;
} catch(error) {
  report.passed=false; report.error=String(error.stack??error); process.exitCode=1;
} finally {
  await close(); await new Promise(resolve=>server.close(resolve));
  fs.writeFileSync(output,JSON.stringify(report,null,2)+'\n');
  assert.equal(path.dirname(root),path.resolve(os.tmpdir()));
  assert.ok(path.basename(root).startsWith('pyscript-browser-'));
  fs.rmSync(root,{recursive:true,force:true,maxRetries:8,retryDelay:250});
}
console.log(JSON.stringify({passed:report.passed,cases:report.cases.length,output,error:report.error},null,2));
