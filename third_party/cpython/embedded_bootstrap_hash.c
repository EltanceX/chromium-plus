// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "Python.h"
#include <windows.h>
#include <bcrypt.h>

extern void ChromiumPythonRandBytes(unsigned char* buffer, size_t length);

// CPython's BCryptGenRandom path cannot initialize in the locked-down renderer.
// Use Chromium's CSPRNG for the hash secret and all _PyOS_URandom calls. Keep
// upstream bytes intact and retain full random entropy (no fixed hash seed).
static NTSTATUS ChromiumPythonGenRandom(BCRYPT_ALG_HANDLE algorithm,
                                       unsigned char* buffer,
                                       ULONG length,
                                       ULONG flags) {
  if (algorithm != NULL || flags != BCRYPT_USE_SYSTEM_PREFERRED_RNG) {
    return (NTSTATUS)0xc000000d;  // STATUS_INVALID_PARAMETER
  }
  ChromiumPythonRandBytes(buffer, length);
  return 0;  // STATUS_SUCCESS
}

#define BCryptGenRandom ChromiumPythonGenRandom
#include "src/Python/bootstrap_hash.c"
