// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This is the page's first author script, including after the reload check.
const firstScriptSnapshot = snapshot();
const savedResults = sessionStorage.getItem('chromiumPlusResults');
const results = savedResults ? JSON.parse(savedResults) : [];

function snapshot() {
  return {
    value: globalThis.chromiumPlus,
    descriptor: Object.getOwnPropertyDescriptor(globalThis, 'chromiumPlus'),
  };
}

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function checkSnapshot(info) {
  assert(info.value === '0.0.1', `Unexpected value: ${info.value}`);
  const descriptor = info.descriptor;
  assert(descriptor?.value === '0.0.1', 'Missing own data property');
  assert(descriptor.writable && descriptor.enumerable && descriptor.configurable,
         'Expected a writable, enumerable, configurable property');
}

async function timed(promise) {
  let timer;
  try {
    return await Promise.race([
      promise,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error('Timed out')), 10000);
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

async function test(name, run) {
  try {
    await run();
    results.push({name, passed: true});
  } catch (error) {
    results.push({name, passed: false, error: String(error)});
  }
  await fetch('/progress', {
    method: 'POST',
    body: JSON.stringify(results.at(-1)),
  });
}

async function checkFrame(hostname, {srcdoc = false, csp = false} = {}) {
  const frame = document.createElement('iframe');
  const token = crypto.randomUUID();
  let listener;
  const message = new Promise(resolve => {
    listener = event => {
      if (event.source === frame.contentWindow && event.data?.token === token) {
        resolve(event.data.snapshot);
      }
    };
    window.addEventListener('message', listener);
  });
  if (srcdoc) {
    frame.srcdoc = `<script>parent.postMessage({token: '${token}',
        snapshot: (${snapshot.toString()})()}, '*');<\/script>`;
  } else {
    const url = new URL('frame.html', location.href);
    url.hostname = hostname;
    url.searchParams.set('token', token);
    if (csp) {
      url.searchParams.set('csp', '1');
    }
    frame.src = url.href;
  }
  document.body.append(frame);
  try {
    checkSnapshot(await timed(message));
  } finally {
    window.removeEventListener('message', listener);
    frame.remove();
  }
}

async function checkWorker(kind, type) {
  const url = `worker.js?kind=${kind}&type=${type}`;
  const worker = kind === 'shared' ?
      new SharedWorker(url, {type, name: crypto.randomUUID()}) :
      new Worker(url, {type});
  const port = kind === 'shared' ? worker.port : worker;
  const message = new Promise((resolve, reject) => {
    port.onmessage = event => resolve(event.data);
    worker.onerror = () => reject(new Error('Worker script failed'));
  });
  try {
    checkSnapshot(await timed(message));
  } finally {
    if (kind === 'shared') {
      port.close();
    } else {
      worker.terminate();
    }
  }
}

async function checkServiceWorker(type) {
  const registration = await navigator.serviceWorker.register(
      `worker.js?kind=service&type=${type}`, {type, scope: `./sw-${type}/`});
  const channel = new MessageChannel();
  try {
    const worker = registration.active || registration.installing ||
        registration.waiting;
    if (worker.state !== 'activated') {
      await timed(new Promise((resolve, reject) => {
        worker.addEventListener('statechange', () => {
          if (worker.state === 'activated') {
            resolve();
          } else if (worker.state === 'redundant') {
            reject(new Error('Service Worker became redundant'));
          }
        });
      }));
    }
    const message = new Promise(resolve => {
      channel.port1.onmessage = event => resolve(event.data);
    });
    worker.postMessage('snapshot', [channel.port2]);
    checkSnapshot(await timed(message));
  } finally {
    channel.port1.close();
    channel.port2.close();
    await registration.unregister();
  }
}

async function run() {
  if (savedResults) {
    sessionStorage.removeItem('chromiumPlusResults');
    await test('reload restores default before first script', () => {
      checkSnapshot(firstScriptSnapshot);
    });
    await fetch('/results', {method: 'POST', body: JSON.stringify(results)});
    return;
  }
  await test('page first script and property descriptor', () => {
    checkSnapshot(firstScriptSnapshot);
  });
  await test('mutation, same-document navigation and deletion', () => {
    globalThis.chromiumPlus = 'changed';
    history.pushState({}, '', '#same-document');
    assert(globalThis.chromiumPlus === 'changed', 'Same-document value reset');
    assert(delete globalThis.chromiumPlus, 'Deletion failed');
    assert(!Object.hasOwn(globalThis, 'chromiumPlus'), 'Property still present');
    globalThis.chromiumPlus = '0.0.1';
  });
  await test('initial about:blank iframe', () => {
    const frame = document.createElement('iframe');
    document.body.append(frame);
    try {
      checkSnapshot({
        value: frame.contentWindow.chromiumPlus,
        descriptor: Object.getOwnPropertyDescriptor(
            frame.contentWindow, 'chromiumPlus'),
      });
    } finally {
      frame.remove();
    }
  });
  await test('initial about:blank popup', () => {
    const popup = window.open('about:blank');
    assert(popup, 'Popup was blocked');
    try {
      checkSnapshot({
        value: popup.chromiumPlus,
        descriptor: Object.getOwnPropertyDescriptor(popup, 'chromiumPlus'),
      });
    } finally {
      popup.close();
    }
  });
  await test('srcdoc first script', () => checkFrame(location.hostname, {
    srcdoc: true,
  }));
  await test('same-origin iframe first script', () => checkFrame('127.0.0.1'));
  await test('cross-site iframe first script', () => checkFrame('localhost'));
  await test('iframe with restrictive script CSP', () => checkFrame(
      location.hostname, {csp: true}));
  for (const type of ['classic', 'module']) {
    await test(`dedicated ${type} worker`, () => checkWorker('dedicated', type));
    await test(`shared ${type} worker`, () => checkWorker('shared', type));
    await test(`service ${type} worker`, () => checkServiceWorker(type));
  }
  sessionStorage.setItem('chromiumPlusResults', JSON.stringify(results));
  globalThis.chromiumPlus = 'changed-before-reload';
  location.reload();
}

run().catch(async error => {
  results.push({name: 'test harness', passed: false, error: String(error)});
  await fetch('/results', {method: 'POST', body: JSON.stringify(results)});
});
