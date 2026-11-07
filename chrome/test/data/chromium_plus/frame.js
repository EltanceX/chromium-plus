// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Capture the value in the frame's first author script.
parent.postMessage({
  token: new URL(location.href).searchParams.get('token'),
  snapshot: {
    value: globalThis.chromiumPlus,
    descriptor: Object.getOwnPropertyDescriptor(globalThis, 'chromiumPlus'),
  },
}, '*');
