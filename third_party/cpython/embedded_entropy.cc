// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstddef>
#include <cstdint>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/rand_util.h"

extern "C" void ChromiumPythonRandBytes(uint8_t* buffer, size_t length) {
  // CPython supplies a writable allocation of exactly this size. RandBytes uses
  // the renderer-compatible ProcessPrng path through BoringSSL on Windows.
  base::RandBytes(UNSAFE_BUFFERS(base::span(buffer, length)));
}
