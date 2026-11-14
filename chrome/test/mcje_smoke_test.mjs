// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Local TCP + native navigation checks. Deterministic SRV injection is covered
// by java_status_query_unittest.cc; PNG decoding uses the isolated service.
// --public-host enables a separate live SLP probe. Its observed fields are
// recorded without fixed version/player/DNS expectations; failures still fail
// the run, after the remaining local shutdown check and cleanup have finished.
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {Resolver} from 'node:dns/promises';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {deflateSync} from 'node:zlib';

const args = process.argv.slice(2);
const option = (name, fallback) =>
    args.find(arg => arg.startsWith(`--${name}=`))?.slice(name.length + 3) ?? fallback;
const binary = path.resolve(option('chrome-binary', 'out/Debug/chrome.exe'));
const output = path.resolve(option('output', 'out/Debug/mcje-smoke-results.json'));
const publicHost = option('public-host', '').toLowerCase();
if (publicHost) assert.match(publicHost, /^[a-z0-9](?:[a-z0-9.-]{0,251}[a-z0-9])?$/i);
const publicOnly = args.includes('--public-only');
if (publicOnly) assert.ok(publicHost,'--public-only requires --public-host');
const chromeArgs = args.filter(arg => arg.startsWith('--chrome-arg='))
    .map(arg => arg.slice('--chrome-arg='.length));
const temporaryRoot = path.resolve(os.tmpdir());
const profile = fs.mkdtempSync(path.join(temporaryRoot, 'mcje-smoke-'));
fs.mkdirSync(path.dirname(output), {recursive: true});
const log = fs.openSync(`${output}.chrome.log`, 'w');
const results = [];
const sockets = new Set();
const records = [];
let publicObservation;
let lastPageState;
let behavior = 'normal';
function readVarInt(bytes, offset = 0) {
  let value = 0;
  for (let i = 0; i < 5; ++i) {
    if (offset + i >= bytes.length) return null;
    const byte = bytes[offset + i];
    if (i === 4 && (byte & 0xf0)) throw new Error('Invalid client VarInt');
    value |= (byte & 0x7f) << (7 * i);
    if (!(byte & 0x80)) return {value, size: i + 1};
  }
  throw new Error('Invalid client VarInt');
}
function varInt(value) {
  const bytes = [];
  do { let byte = value & 0x7f; value >>>= 7; bytes.push(byte | (value ? 0x80 : 0)); } while (value);
  return Buffer.from(bytes);
}
function statusFrame(value) {
  const json = Buffer.from(JSON.stringify(value));
  const packet = Buffer.concat([Buffer.from([0]), varInt(json.length), json]);
  return Buffer.concat([varInt(packet.length), packet]);
}
function pngChunk(type, data) {
  const name = Buffer.from(type);
  let crc = 0xffffffff;
  for (const byte of Buffer.concat([name, data])) {
    crc ^= byte;
    for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  const length = Buffer.alloc(4); length.writeUInt32BE(data.length);
  const checksum = Buffer.alloc(4); checksum.writeUInt32BE((crc ^ 0xffffffff) >>> 0);
  return Buffer.concat([length, name, data, checksum]);
}
function makeIcon(size = 64) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0); ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8; ihdr[9] = 6;
  const pixels = Buffer.alloc(size * (size * 4 + 1));
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x) {
      const offset = y * (size * 4 + 1) + 1 + x * 4;
      const shade = ((x >> 3) + (y >> 3)) % 2 ? 14 : 0;
      const color = y < size / 2 ? [68 + shade, 159 + shade, 87 + shade] :
                                 [125 + shade, 88 + shade, 55 + shade];
      pixels.set([...color, x < 8 || x >= size - 8 || y < 8 || y >= size - 8 ? 0 : 220], offset);
    }
  }
  return Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]),
    pngChunk('IHDR', ihdr), pngChunk('tEXt', Buffer.from('Comment\0REMOTE_ICON_METADATA')),
    pngChunk('IDAT', deflateSync(pixels)), pngChunk('IEND', Buffer.alloc(0))]);
}
const originalIcon = makeIcon();
const iconUrl = bytes => 'data:image/png;base64,' + bytes.toString('base64');
const iconVariants = {
  valid: iconUrl(originalIcon),
  missing: undefined,
  type: 42,
  base64: 'data:image/png;base64,NOT!BASE64',
  whitespace: 'data:image/png;base64, ' + originalIcon.toString('base64'),
  mime: 'data:image/svg+xml;base64,PHN2Zz4=',
  remote: 'https://example.test/untrusted.png',
  dimensions: iconUrl(makeIcon(32)),
  signature: iconUrl(Buffer.from('this is not a PNG')),
  corrupt: iconUrl(originalIcon.subarray(0, 33)),
  encodedOversize: 'data:image/png;base64,' + 'A'.repeat(90000),
  decodedOversize: iconUrl(Buffer.alloc(65537)),
};
let iconVariant = 'valid';
const statusValue = {
  description: {text:'§aJava 世界 <img src=x onerror="globalThis.mcjeInjected=1"> &',
                extra:[{text:'\n第二行'}]},
  version:{name:'1.21.local', protocol:767}, players:{online:12, max:100},
  favicon:iconVariants.valid,
};
function accept(socket) {
  ++connections;
  sockets.add(socket);
  const record = {mode:behavior, closed:false, frames:[], port:socket.localPort};
  records.push(record);
  socket.on('error', () => {});
  socket.on('close', () => { record.closed = true; sockets.delete(socket); });
  let buffered = Buffer.alloc(0);
  let responded = false;
  socket.on('data', async bytes => {
    try {
      buffered = Buffer.concat([buffered, bytes]);
      assert.ok(buffered.length < 4096);
      for (;;) {
        const length = readVarInt(buffered);
        if (!length || buffered.length < length.size + length.value) break;
        record.frames.push(buffered.subarray(length.size, length.size + length.value));
        buffered = buffered.subarray(length.size + length.value);
      }
      if (record.frames.length < 3 || responded) return;
      responded = true;
      assert.equal(record.frames.length, 3);
      const hello = record.frames[0];
      let offset = 0;
      const id = readVarInt(hello, offset); offset += id.size;
      const version = readVarInt(hello, offset); offset += version.size;
      const hostLength = readVarInt(hello, offset); offset += hostLength.size;
      record.host = hello.subarray(offset, offset + hostLength.value).toString('utf8');
      offset += hostLength.value;
      record.logicalPort = hello.readUInt16BE(offset); offset += 2;
      const nextState = readVarInt(hello, offset); offset += nextState.size;
      assert.equal(id.value, 0); assert.equal(version.value, 47);
      assert.equal(nextState.value, 1); assert.equal(offset, hello.length);
      assert.deepEqual(record.frames[1], Buffer.from([0]));
      assert.equal(record.frames[2].length, 9); assert.equal(record.frames[2][0], 1);
      record.handshakeValid = true;
      const pong = Buffer.concat([Buffer.from([9]), record.frames[2]]);
      const status = statusFrame({...statusValue, favicon:iconVariants[iconVariant]});
      if (record.mode === 'silent') return;
      if (record.mode === 'no-pong') { socket.write(status); return; }
      if (record.mode === 'close') { socket.end(status); return; }
      if (record.mode === 'bad-json') { socket.end(statusFrame({description:false})); return; }
      if (record.mode === 'truncated') { socket.end(status.subarray(0, 8)); return; }
      if (record.mode === 'pong-first') { socket.write(Buffer.concat([pong, status])); return; }
      if (record.mode === 'wrong-pong') {
        const wrong = Buffer.from(pong); wrong[9] ^= 1;
        socket.write(Buffer.concat([wrong, status, pong])); return;
      }
      const response = Buffer.concat([status, pong]);
      if (record.mode === 'split') {
        for (let offset = 0; offset < response.length && !socket.destroyed; offset += 7) {
          socket.write(response.subarray(offset, offset + 7)); await delay(3);
        }
      } else { socket.write(response); }
    } catch (error) { record.error = String(error.stack ?? error); socket.destroy(); }
  });
}
const server = net.createServer(accept);
const ipv6Server = net.createServer(accept);
let connections = 0;
let browser;
let ws;

async function waitFor(predicate, timeout = 12000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    try {
      const value = await predicate();
      if (value) return value;
    } catch (error) {
      if (!/Inspected target navigated or closed|Execution context was destroyed|Cannot find context/.test(error.message)) {
        throw error;
      }
    }
    await delay(50);
  }
  throw new Error(`Condition timed out after ${timeout} ms`);
}

async function check(name, callback) {
  const start = Date.now();
  try {
    const detail = await callback();
    results.push({name, passed: true, elapsedMs: Date.now() - start, detail});
    console.log(`PASS ${name}`);
  } catch (error) {
    results.push({name, passed: false, elapsedMs: Date.now() - start,
                  error: String(error.stack ?? error), page:lastPageState});
    throw error;
  }
}

async function observeDns(host) {
  // These are observations from Node's resolver, not injected Chrome results.
  // They help diagnose local DNS/proxy differences without changing either.
  const resolver = new Resolver({timeout:1500, tries:1});
  try {
    return await Promise.all(['SRV','A','AAAA'].map(async type => {
      const name=type==='SRV' ? `_minecraft._tcp.${host}` : host;
      try { return {type,name,records:await resolver.resolve(name,type)}; }
      catch (error) { return {type,name,error:error.code}; }
    }));
  } finally { resolver.cancel(); }
}

try {
  // Only the disposable test profile needs the omnibox diagnostics WebUI.
  fs.writeFileSync(path.join(profile, 'Local State'),
                   JSON.stringify({internal_only_uis_enabled: true}));
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const targetUrl = `mcje://127.0.0.1:${server.address().port}/`;
  browser = spawn(binary, [
    '--headless=new', '--no-first-run', '--no-default-browser-check',
    '--disable-background-networking', '--remote-debugging-port=0',
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
  const requestUrls = new Map();
  const failures = [];
  ws.onmessage = event => {
    const message = JSON.parse(event.data);
    if (message.id) {
      pending.get(message.id)?.(message);
      pending.delete(message.id);
    } else if (message.method === 'Network.responseReceived') {
      responses.push(message.params.response);
    } else if (message.method === 'Network.requestWillBeSent') {
      requestUrls.set(message.params.requestId, message.params.request.url);
    } else if (message.method === 'Network.loadingFailed') {
      failures.push({url: requestUrls.get(message.params.requestId), ...message.params});
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
      }, 15000);
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
    const state=await evaluate(`({url:location.href, origin:location.origin,
      ready:document.readyState, text:document.body?.innerText ?? '',
      state:document.querySelector('#address-state')?.textContent,
      host:document.querySelector('#logical-host')?.textContent,
      port:document.querySelector('#logical-port')?.textContent,
      portSource:document.querySelector('#port-source')?.textContent,
      query:document.querySelector('#query-state')?.textContent,
      motd:document.querySelector('#motd')?.textContent,
      rtt:document.querySelector('#rtt')?.textContent,
      endpoint:document.querySelector('#endpoint')?.textContent,
      srvTarget:document.querySelector('#srv-target')?.textContent,
      discovery:document.querySelector('#discovery')?.textContent,
      attempts:document.querySelector('#attempts')?.textContent,
      version:document.querySelector('#version-name')?.textContent,
      protocol:document.querySelector('#protocol-version')?.textContent,
      playersOnline:document.querySelector('#players-online')?.textContent,
      playersMax:document.querySelector('#players-max')?.textContent,
      latencyDiagnostic:document.querySelector('#latency-diagnostic')?.textContent,
      error:document.querySelector('#query-error')?.textContent,
      summary:document.querySelector('#json-summary')?.textContent,
      images:document.images.length,
      icon:document.querySelector('#server-icon') && {
        complete:document.querySelector('#server-icon').complete,
        width:document.querySelector('#server-icon').naturalWidth,
        height:document.querySelector('#server-icon').naturalHeight,
        src:document.querySelector('#server-icon').src},
      placeholder:!!document.querySelector('#icon-placeholder'),
      iconState:document.querySelector('#icon-state')?.textContent,
      completedAt:document.querySelector('#completed-at')?.textContent,
      injected:!!globalThis.mcjeInjected,
      scripts:document.scripts.length, secure:isSecureContext})`);
    lastPageState=state;
    return state;
  }
  async function navigate(url, expectedUrl = url) {
    const navigation = await send('Page.navigate', {url});
    if (navigation.errorText) throw new Error(`Navigation failed: ${navigation.errorText}`);
    let state;
    try {
      return await waitFor(async () => {
        state = await pageState();
        return state.url === expectedUrl && state.ready === 'complete' && state.state && state;
      });
    } catch (error) {
      throw new Error(`Navigation to ${url}: ${error.message}; last page: ${JSON.stringify(state)}`);
    }
  }
  const header = (response, name) => Object.entries(response.headers)
      .find(([key]) => key.toLowerCase() === name.toLowerCase())?.[1];
  await send('Page.enable');
  await send('Network.enable');

  if (!publicOnly) {
  await check('real omnibox classifier recognizes MCJE URLs', async () => {
    const navigation = await send('Page.navigate', {url: 'chrome://omnibox-internals/'});
    assert.ok(!navigation.errorText, navigation.errorText);
    let diagnosticPage;
    try {
      await waitFor(async () => {
        diagnosticPage = await pageState();
        return diagnosticPage.url === 'chrome://omnibox-internals/' &&
            diagnosticPage.ready !== 'loading';
      }, 30000);
    } catch (error) {
      throw new Error(`Omnibox diagnostics did not load: ${error.message}; ${JSON.stringify(diagnosticPage)}`);
    }
    const result = await evaluate(`(async () => {
      const m = await import('/omnibox_internals.mojom-webui.js');
      const router = new m.OmniboxInternalsPageCallbackRouter();
      const handler = m.OmniboxInternalsPageHandler.getRemote();
      handler.setClientPage(router.$.bindNewPipeAndPassRemote());
      globalThis.mcjeOmniboxProbe = {router, handler};
      const input = ${JSON.stringify(targetUrl)};
      return new Promise(resolve => {
        router.handleNewAutocompleteResponse.addListener((controller, response) => {
          const matches = response.combinedResults.map(match => ({
            url:match.destinationUrl, search:match.isSearchType,
            default:match.allowedToBeDefaultMatch}));
          if (controller === m.AutocompleteControllerType.kDebug &&
              response.inputText === input && (response.done ||
              matches.some(match => match.url === input && !match.search && match.default))) {
            resolve({type:response.type, matches});
          }
        });
        handler.startOmniboxQuery(input, true, input.length, false, true, false, '', 4);
      });
    })()`);
    assert.equal(result.type.toLowerCase(), 'url');
    assert.ok(result.matches.some(match => match.url === targetUrl && !match.search && match.default),
              JSON.stringify(result));
    return result;
  });
  await check('native TCP query, handshake, escaped MOTD and opaque origin', async () => {
    const state = await navigate(targetUrl);
    assert.equal(state.query, '查询成功');
    assert.equal(state.origin, 'null'); assert.equal(state.secure, false);
    assert.equal(state.scripts, 0); assert.equal(state.images, 1);
    assert.equal(state.placeholder, false);
    assert.equal(state.icon.complete, true);
    assert.equal(state.icon.width, 64); assert.equal(state.icon.height, 64);
    assert.match(state.icon.src, /^data:image\/png;base64,/);
    assert.notEqual(state.icon.src, iconVariants.valid);
    assert.ok(!Buffer.from(state.icon.src.split(',')[1], 'base64').includes(Buffer.from('REMOTE_ICON_METADATA')));
    assert.match(state.completedAt, / UTC$/);
    const alpha = await evaluate(`(() => {
      const canvas=document.createElement('canvas');canvas.width=64;canvas.height=64;
      const ctx=canvas.getContext('2d');ctx.drawImage(document.querySelector('#server-icon'),0,0);
      return [ctx.getImageData(0,0,1,1).data[3],ctx.getImageData(32,32,1,1).data[3]];
    })()`);
    assert.equal(alpha[0],0); assert.ok(alpha[1]>0 && alpha[1]<255);
    assert.equal(state.injected, false);
    assert.equal(state.motd, 'Java 世界 <img src=x onerror="globalThis.mcjeInjected=1"> &\n第二行');
    assert.match(state.rtt, / ms$/);
    assert.ok(!state.summary.includes('REMOTE_ICON_METADATA'));
    assert.ok(!state.summary.includes(iconVariants.valid));
    const record = records.at(-1);
    assert.equal(record.host, '127.0.0.1');
    assert.equal(record.logicalPort, server.address().port);
    assert.equal(record.handshakeValid, true, record.error);
    const response = responses.find(response => response.url === state.url);
    assert.match(header(response, 'Content-Security-Policy'), /default-src 'none'/);
    assert.match(header(response, 'Content-Security-Policy'), /img-src data:/);
    assert.equal(header(response, 'Cache-Control'), 'no-store');
    assert.equal(header(response, 'Referrer-Policy'), 'no-referrer');
    assert.equal(await evaluate('typeof globalThis.chrome?.send'), 'undefined');
    return state;
  });
  const screenshot = option('screenshot', '');
  if (screenshot) {
    const captured = await send('Page.captureScreenshot', {format:'png',captureBeyondViewport:true});
    fs.writeFileSync(path.resolve(screenshot), Buffer.from(captured.data,'base64'));
  }
  await check('CSP blocks injected scripts and remote images', async () => {
    const remoteImage=`http://127.0.0.1:${server.address().port}/remote.png`;
    const previous=connections, failureStart=failures.length;
    await evaluate(`{const script=document.createElement('script');
      script.textContent='globalThis.mcjeInjected=1';document.body.append(script);
      const img=document.createElement('img');img.src=${JSON.stringify(remoteImage)};
      document.body.append(img);}`);
    assert.equal(await evaluate('!!globalThis.mcjeInjected'), false);
    const failure=await waitFor(() => failures.slice(failureStart)
        .find(failure => failure.url===remoteImage));
    assert.equal(failure.blockedReason,'csp');
    assert.equal(connections,previous);
  });
  for (const variant of Object.keys(iconVariants).filter(key => key !== 'valid')) {
    await check(`optional ${variant} icon uses placeholder and preserves status`, async () => {
      iconVariant=variant;
      await send('Page.navigate',{url:'about:blank'});
      const state=await navigate(targetUrl);
      assert.equal(state.query,'查询成功');
      assert.equal(state.images,0); assert.equal(state.icon,null);
      assert.equal(state.placeholder,true); assert.ok(state.iconState);
      assert.ok(state.motd.includes('Java 世界')); assert.equal(state.injected,false);
      return {iconState:state.iconState};
    });
  }
  iconVariant='valid';
  await check('DNS target retains logical hostname in handshake', async () => {
    const url = `mcje://localhost:${server.address().port}/`;
    const state = await navigate(url.replace('localhost','LOCALHOST'),url);
    assert.equal(state.query,'查询成功');
    assert.equal(state.host,'localhost');
    assert.equal(records.at(-1).host,'localhost');
    assert.equal(records.at(-1).logicalPort,server.address().port);
  });
  await check('reload starts a new TCP query', async () => {
    await navigate(targetUrl);
    const previous = connections;
    const time = await evaluate('performance.timeOrigin');
    await send('Page.reload',{ignoreCache:true});
    const state = await waitFor(async () => {
      const value=await pageState();
      return await evaluate('performance.timeOrigin') !== time &&
          value.ready==='complete' && value.query && value;
    });
    assert.equal(state.query,'查询成功');
    assert.equal(state.portSource,'显式指定');
    assert.equal(connections,previous+1);
  });
  await check('same-document fragment makes no connection', async () => {
    const previous=connections;
    const time=await evaluate('performance.timeOrigin');
    await navigate(targetUrl+'#details');
    assert.equal(await evaluate('performance.timeOrigin'),time);
    assert.equal(connections,previous);
  });
  await check('invalid URLs never connect', async () => {
    const previous=connections;
    for (const url of [targetUrl+'path',targetUrl+'?query',targetUrl+'#fragment',
        targetUrl.replace('mcje://','mcje://user:password@'),'mcje://127.0.0.1:0/']) {
      await send('Page.navigate',{url:'about:blank'});
      const state=await navigate(url,url.replace('user:password@',''));
      assert.equal(state.state,'地址无效');
    }
    assert.equal(connections,previous);
  });
  await check('subframe and fetch cannot open TCP query', async () => {
    await send('Page.navigate',{url:'about:blank'});
    await waitFor(async () => (await pageState()).url==='about:blank');
    const previous=connections, failureStart=failures.length;
    await evaluate(`{const frame=document.createElement('iframe');
      frame.src=${JSON.stringify(targetUrl)};document.body.append(frame);}`);
    await waitFor(() => failures.slice(failureStart).find(f => f.url===targetUrl));
    assert.equal(await evaluate(`fetch(${JSON.stringify(targetUrl)}).then(()=>false,()=>true)`),true);
    await delay(200);
    assert.equal(connections,previous);
  });
  for (const mode of ['split','pong-first','wrong-pong']) {
    await check(`${mode} response keeps valid state and matched Pong`,async () => {
      behavior=mode;
      await send('Page.navigate',{url:'about:blank'});
      const state=await navigate(targetUrl);
      assert.equal(state.query,'查询成功'); assert.match(state.rtt,/ ms$/);
      assert.equal(state.injected,false);
    });
  }
  await check('valid status without Pong ends after short wait',async () => {
    behavior='no-pong';
    await send('Page.navigate',{url:'about:blank'});
    const start=Date.now();
    const state=await navigate(targetUrl);
    assert.equal(state.query,'查询成功'); assert.equal(state.rtt,'未取得');
    assert.ok(Date.now()-start<3000);
    await waitFor(() => records.at(-1).closed);
  });
  await check('connection close after valid status preserves MOTD',async () => {
    behavior='close';
    await send('Page.navigate',{url:'about:blank'});
    const state=await navigate(targetUrl);
    assert.equal(state.query,'查询成功'); assert.equal(state.rtt,'未取得');
    assert.ok(state.motd.includes('Java 世界'));
  });
  for (const mode of ['bad-json','truncated']) {
    await check(`${mode} yields a local query error`,async () => {
      behavior=mode;
      await send('Page.navigate',{url:'about:blank'});
      const state=await navigate(targetUrl);
      assert.equal(state.query,'查询失败'); assert.ok(state.error);
    });
  }
  await check('silent server respects five-second total budget',async () => {
    behavior='silent';
    await send('Page.navigate',{url:'about:blank'});
    const previous=connections, start=Date.now();
    const state=await navigate(targetUrl);
    assert.equal(state.query,'查询失败'); assert.match(state.error,/5 秒/);
    assert.ok(Date.now()-start>=4500 && Date.now()-start<8000);
    assert.equal(connections,previous+1);
    await waitFor(() => records.at(-1).closed);
  });
  await check('navigation cancellation closes socket without retry',async () => {
    behavior='silent';
    await send('Page.navigate',{url:'about:blank'});
    const previous=connections;
    const navigation=send('Page.navigate',{url:targetUrl});
    await waitFor(() => connections===previous+1 && records.at(-1).handshakeValid);
    const record=records.at(-1);
    await send('Page.navigate',{url:'about:blank'});
    await navigation;
    await waitFor(() => record.closed);
    await delay(1200);
    assert.equal(connections,previous+1);
    assert.equal((await pageState()).url,'about:blank');
  });
  await check('closing a pending tab cancels TCP',async () => {
    const previous=connections;
    const target=await send('Target.createTarget',{url:targetUrl});
    await waitFor(() => connections===previous+1);
    const record=records.at(-1);
    await send('Target.closeTarget',{targetId:target.targetId});
    await waitFor(() => record.closed);
    assert.equal(connections,previous+1);
  });
  await check('explicit IPv6 endpoint',async () => {
    behavior='normal';
    await new Promise((resolve,reject) => {
      ipv6Server.once('error',reject);ipv6Server.listen(0,'::1',resolve);
    });
    const url=`mcje://[::1]:${ipv6Server.address().port}/`;
    const state=await navigate(url);
    assert.equal(state.query,'查询成功');assert.equal(state.host,'::1');
    assert.equal(records.at(-1).host,'::1');
  });
  await check('back and forward preserve native results',async () => {
    await navigate(targetUrl);
    await send('Page.navigate',{url:'about:blank'});
    await waitFor(async () => (await pageState()).url==='about:blank');
    await evaluate('setTimeout(() => history.back(),50)');
    await waitFor(async () => {
      const state=await pageState();return state.url===targetUrl && state.query==='查询成功';
    });
    await evaluate('setTimeout(() => history.forward(),50)');
    await waitFor(async () => (await pageState()).url==='about:blank');
  });
  await check('all local handshakes have expected frame order',async () => {
    assert.ok(records.length>5);
    assert.ok(records.every(record => !record.error),JSON.stringify(records.filter(r => r.error)));
    assert.ok(records.filter(r => r.frames.length>=3).every(r => r.handshakeValid));
  });
  }
  if (publicHost) {
    const start=Date.now(), name='public SLP status and optional icon';
    publicObservation={host:publicHost, observedAt:new Date().toISOString(),
                       dns:await observeDns(publicHost), status:'not-completed'};
    try {
      await send('Page.navigate',{url:'about:blank'});
      const state=await navigate(`mcje://${publicHost}/`);
      publicObservation.page=state;
      publicObservation.status=state.query==='查询成功' ? 'success' : 'query-failed';
      const publicScreenshot=option('public-screenshot','');
      if (publicScreenshot) {
        const captured=await send('Page.captureScreenshot',
                                 {format:'png',captureBeyondViewport:true});
        fs.mkdirSync(path.dirname(path.resolve(publicScreenshot)),{recursive:true});
        fs.writeFileSync(path.resolve(publicScreenshot),Buffer.from(captured.data,'base64'));
        publicObservation.screenshot=path.resolve(publicScreenshot);
      }
      assert.equal(state.origin,'null'); assert.equal(state.secure,false);
      assert.equal(state.scripts,0); assert.equal(state.injected,false);
      assert.equal(state.state,'地址已识别');
      const response=responses.findLast(response => response.url===state.url);
      assert.match(header(response,'Content-Security-Policy'),/img-src data:/);
      assert.equal(header(response,'Cache-Control'),'no-store');
      assert.equal(state.query,'查询成功',state.error);
      assert.equal(typeof state.motd,'string');
      assert.ok(state.summary); assert.match(state.completedAt,/ UTC$/);
      if (state.icon) {
        assert.equal(state.images,1); assert.equal(state.icon.complete,true);
        assert.equal(state.icon.width,64); assert.equal(state.icon.height,64);
        assert.match(state.icon.src,/^data:image\/png;base64,/);
      } else {
        assert.equal(state.images,0); assert.equal(state.placeholder,true);
      }
      results.push({name,passed:true,elapsedMs:Date.now()-start,detail:publicObservation});
      console.log(`PASS ${name}`);
    } catch (error) {
      const diagnostic=String(error.stack ?? error);
      publicObservation.error=diagnostic;
      results.push({name,passed:false,elapsedMs:Date.now()-start,error:diagnostic,
                    detail:publicObservation});
      console.error(`FAIL ${name}: ${diagnostic}`);
      process.exitCode=1;
    }
  }
  if (publicOnly) {
    await send('Browser.close').catch(error => {
      assert.match(error.message,/CDP connection closed/);
    });
    await waitFor(() => browser.exitCode!==null);
  } else {
  await check('browser shutdown cancels an outstanding query',async () => {
    behavior='silent';
    const previous=connections;
    const navigation=send('Page.navigate',{url:targetUrl}).catch(() => {});
    await waitFor(() => connections===previous+1 && records.at(-1).handshakeValid);
    const record=records.at(-1);
    await send('Browser.close').catch(error => {
      assert.match(error.message,/CDP connection closed/);
    });
    await navigation;
    await waitFor(() => record.closed && browser.exitCode!==null);
    assert.equal(connections,previous+1);
  });
  }
} catch (error) {
  console.error(error.stack ?? error);
  if (!results.some(result => !result.passed)) {
    results.push({name: 'test harness', passed: false, error: String(error.stack ?? error)});
  }
  process.exitCode = 1;
} finally {
  ws?.close();
  if (browser?.pid && browser.exitCode === null) {
    if (process.platform === 'win32') {
      spawnSync('taskkill', ['/PID', String(browser.pid), '/T', '/F'], {windowsHide: true, stdio: 'ignore'});
    } else {
      browser.kill('SIGKILL');
    }
    await delay(200);
  }
  for (const socket of sockets) socket.destroy();
  for (const listener of [server, ipv6Server]) {
    if (listener.listening) await new Promise(resolve => listener.close(resolve));
  }
  fs.closeSync(log);
  fs.writeFileSync(output, JSON.stringify({binary, chromeArgs, phase:5, publicOnly, results,
      publicObservation,
      connections, records:records.map(({frames,...record}) => record)}, null, 2));
  assert.equal(path.dirname(path.resolve(profile)), temporaryRoot);
  assert.ok(path.basename(profile).startsWith('mcje-smoke-'));
  fs.rmSync(profile, {recursive: true, force: true, maxRetries: 10, retryDelay: 300});
  console.log(`Results: ${output}`);
}
