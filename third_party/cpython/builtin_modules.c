// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "Python.h"

#define MODULES(V) \
  V(_ast, PyInit__ast) \
  V(_abc, PyInit__abc) \
  V(_codecs, PyInit__codecs) \
  V(_weakref, PyInit__weakref) \
  V(_thread, PyInit__thread) \
  V(_io, PyInit__io) \
  V(_imp, PyInit__imp) \
  V(_string, PyInit__string) \
  V(_tokenize, PyInit__tokenize) \
  V(_warnings, _PyWarnings_Init) \
  V(_signal, PyInit__signal) \
  V(_tracemalloc, PyInit__tracemalloc) \
  V(atexit, PyInit_atexit) \
  V(faulthandler, PyInit_faulthandler) \
  V(gc, PyInit_gc) \
  V(marshal, PyMarshal_Init) \
  V(math, PyInit_math) \
  V(nt, PyInit_nt) \
  V(winreg, PyInit_winreg) \
  V(time, PyInit_time)

#define DECLARE(name, function) extern PyObject* function(void);
MODULES(DECLARE)
#undef DECLARE
#define ENTRY(name, function) {#name, function},
struct _inittab _PyImport_Inittab[] = {
    MODULES(ENTRY)
    {"builtins", NULL}, {"sys", NULL}, {0, 0}};
#undef ENTRY
