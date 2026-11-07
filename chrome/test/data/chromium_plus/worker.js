// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Capture before any event handler or asynchronous work runs.
const initialSnapshot = {
  value: globalThis.chromiumPlus,
  descriptor: Object.getOwnPropertyDescriptor(globalThis, 'chromiumPlus'),
};

if ('clients' in self) {
  self.addEventListener('install', event => event.waitUntil(self.skipWaiting()));
  self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));
  self.addEventListener('message', event => {
    event.ports[0].postMessage(initialSnapshot);
  });
} else if ('onconnect' in self) {
  self.addEventListener('connect', event => {
    event.ports[0].postMessage(initialSnapshot);
  });
} else {
  self.postMessage(initialSnapshot);
}
