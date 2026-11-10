// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_CANVAS_CANVAS2D_PYTHON_CANVAS_BRIDGE_IMPL_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_CANVAS_CANVAS2D_PYTHON_CANVAS_BRIDGE_IMPL_H_

#include "base/memory/raw_ref.h"
#include "base/time/time.h"
#include "third_party/blink/renderer/core/html/python_canvas_bridge.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_vector.h"
#include "third_party/blink/renderer/platform/wtf/deque.h"

namespace base {
class TickClock;
}
namespace blink {
class CanvasRenderingContext2D;
class PythonCanvasEntry;

struct PythonCanvasCost {
  WebPythonCanvasStatistics statistics;
  uint64_t work = 0;
};
// Pure preflight: never mutates a native context or allocates a backing store.
MODULES_EXPORT WebPythonDomResult
ValidatePythonCanvasBatch(const std::vector<WebPythonCanvasCommand>&,
                          int width,
                          int height,
                          PythonCanvasCost&);

class MODULES_EXPORT PythonCanvasDrawingBudget {
 public:
  explicit PythonCanvasDrawingBudget(const base::TickClock*);
  void BeginExecution();
  WebPythonDomResult Consume(const PythonCanvasCost&);

 private:
  struct Usage {
    base::TimeTicks time;
    PythonCanvasCost cost;
  };
  raw_ref<const base::TickClock> clock_;
  PythonCanvasCost execution_;
  PythonCanvasCost window_;
  Deque<Usage> usages_;
  bool stopped_ = false;
};

class MODULES_EXPORT PythonCanvasBridgeImpl final : public PythonCanvasBridge {
 public:
  static PythonCanvasBridge* Create(Document&, uint64_t, uint64_t);
  PythonCanvasBridgeImpl(Document&, uint64_t, uint64_t);
  WebPythonCanvasResult GetContext(HTMLCanvasElement&) override;
  WebPythonCanvasResult CreateSurface(uint32_t, uint32_t) override;
  WebPythonCanvasResult Resize(WebPythonCanvasHandle,
                               uint32_t,
                               uint32_t) override;
  WebPythonDomResult Release(WebPythonCanvasHandle) override;
  WebPythonCanvasResult Submit(WebPythonCanvasHandle,
                               const std::vector<WebPythonCanvasCommand>&,
                               base::FunctionRef<bool()>) override;
  void BeginExecution() override { budget_.BeginExecution(); }
  void Dispose() override;
  void Trace(Visitor*) const override;

 private:
  PythonCanvasEntry* Resolve(WebPythonCanvasHandle);
  WebPythonCanvasResult Acquire(HTMLCanvasElement&, bool owned);
  bool Fits(uint32_t, uint32_t, const PythonCanvasEntry* except = nullptr);
  bool Execute(CanvasRenderingContext2D&,
               const std::vector<WebPythonCanvasCommand>&,
               base::FunctionRef<bool()>);
  WeakMember<Document> document_;
  const uint64_t document_id_;
  const uint64_t generation_;
  uint64_t version_ = 0;
  bool disposed_ = false;
  HeapVector<Member<PythonCanvasEntry>> entries_;
  PythonCanvasDrawingBudget budget_;
};
}  // namespace blink
#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_CANVAS_CANVAS2D_PYTHON_CANVAS_BRIDGE_IMPL_H_
