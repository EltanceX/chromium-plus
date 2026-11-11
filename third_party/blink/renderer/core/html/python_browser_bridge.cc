// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/python_browser_bridge.h"

#include "base/check.h"

namespace blink {
namespace {
PythonBrowserBridge::Factory g_factory = nullptr;
}
void PythonBrowserBridge::Init(Factory factory) {
  DCHECK(!g_factory);
  g_factory = factory;
}
PythonBrowserBridge* PythonBrowserBridge::Create(
    Document& document,
    base::RepeatingCallback<bool()> allowed) {
  return g_factory ? g_factory(document, std::move(allowed)) : nullptr;
}
}  // namespace blink
