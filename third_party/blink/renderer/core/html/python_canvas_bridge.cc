// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/python_canvas_bridge.h"

#include "base/check.h"

namespace blink {
namespace {
PythonCanvasBridge::Factory g_factory = nullptr;
}
void PythonCanvasBridge::Init(Factory factory) {
  DCHECK(!g_factory);
  g_factory = factory;
}
PythonCanvasBridge* PythonCanvasBridge::Create(Document& d,
                                               uint64_t id,
                                               uint64_t generation) {
  return g_factory ? g_factory(d, id, generation) : nullptr;
}
}  // namespace blink
