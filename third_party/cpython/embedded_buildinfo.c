// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Chromium clears __DATE__/__TIME__ for reproducibility. Keep CPython's build
// identity deterministic without changing upstream sources or Windows headers.
#include "Python.h"
#define DATE "embedded"
#define TIME "00:00:00"
#include "src/Modules/getbuildinfo.c"
