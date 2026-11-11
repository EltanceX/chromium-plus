// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PYTHON_BROWSER_BRIDGE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PYTHON_BROWSER_BRIDGE_H_

#include "base/functional/callback.h"
#include "third_party/blink/public/web/web_python_script_runner.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"

namespace blink {
class Document;
class Element;

// Document-owned native platform services. No Python pointers cross this API.
class CORE_EXPORT PythonBrowserBridge
    : public GarbageCollected<PythonBrowserBridge> {
 public:
  using Factory = PythonBrowserBridge* (*)(Document&,
                                           base::RepeatingCallback<bool()>);
  static void Init(Factory);
  static PythonBrowserBridge* Create(Document&,
                                     base::RepeatingCallback<bool()>);
  virtual ~PythonBrowserBridge() = default;
  virtual WebPythonBrowserResult Run(const WebPythonBrowserRequest&,
                                     Element*) = 0;
  virtual void Dispose() = 0;
  // Native input/lifecycle reset; no script entry, allocation or Python calls.
  virtual void Silence() = 0;
  virtual void Trace(Visitor*) const {}
};
}  // namespace blink
#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PYTHON_BROWSER_BRIDGE_H_
