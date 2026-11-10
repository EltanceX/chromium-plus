// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PYTHON_CANVAS_BRIDGE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PYTHON_CANVAS_BRIDGE_H_

#include "base/functional/function_ref.h"
#include "third_party/blink/public/web/web_python_script_runner.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"

namespace blink {
class Document;
class HTMLCanvasElement;

// Modules supplies Canvas 2D without introducing a core -> modules dependency.
// A bridge belongs to one controller generation, and never owns Python data.
class CORE_EXPORT PythonCanvasBridge
    : public GarbageCollected<PythonCanvasBridge> {
 public:
  using Factory = PythonCanvasBridge* (*)(Document&, uint64_t, uint64_t);
  static void Init(Factory);
  static PythonCanvasBridge* Create(Document&, uint64_t, uint64_t);
  virtual ~PythonCanvasBridge() = default;
  virtual WebPythonCanvasResult GetContext(HTMLCanvasElement&) = 0;
  virtual WebPythonCanvasResult CreateSurface(uint32_t, uint32_t) = 0;
  virtual WebPythonCanvasResult Resize(WebPythonCanvasHandle,
                                       uint32_t,
                                       uint32_t) = 0;
  virtual WebPythonDomResult Release(WebPythonCanvasHandle) = 0;
  virtual WebPythonCanvasResult Submit(
      WebPythonCanvasHandle,
      const std::vector<WebPythonCanvasCommand>&,
      base::FunctionRef<bool()> active) = 0;
  virtual void BeginExecution() = 0;
  virtual void Dispose() = 0;
  virtual void Trace(Visitor*) const {}
};
}  // namespace blink
#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PYTHON_CANVAS_BRIDGE_H_
