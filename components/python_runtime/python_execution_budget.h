// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PYTHON_RUNTIME_PYTHON_EXECUTION_BUDGET_H_
#define COMPONENTS_PYTHON_RUNTIME_PYTHON_EXECUTION_BUDGET_H_

#include <Python.h>

#include <deque>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/time/time.h"
#include "components/python_runtime/python_runtime.h"

namespace base {
class TickClock;
}

namespace native_python {

// Interpreter-local state; never stored in a process-global Python object.
// These are cooperative checks, not a hard limit on native work or memory.
class ExecutionBudget {
 public:
  explicit ExecutionBudget(const base::TickClock* clock);
  ~ExecutionBudget();
  bool Begin(DomHost* host,
             ExecutionKind kind = ExecutionKind::kInitialization);
  bool CanBeginCallback(ExecutionKind kind = ExecutionKind::kCallback);
  void Finish();
  void BeginCleanup();
  bool Check();
  bool CheckTime();
  void Stop(StopReason reason);
  bool RaiseIfStopped(bool check_host = true);
  bool TraceCheckpoint(int event);
  bool ConsumeOutput(size_t bytes);
  bool ConsumeDomCall();
  size_t OutputRemaining() const;
  StopReason reason() const { return reason_; }
  const char* Message() const;
  bool cleanup_exhausted() const { return cleanup_exhausted_; }
  DomHost* host() const { return host_; }
  bool executing() const { return mode_ == Mode::kExecute; }

 private:
  enum class Mode { kIdle, kExecute, kCleanup };
  const raw_ref<const base::TickClock> clock_;
  raw_ptr<DomHost> host_ = nullptr;
  Mode mode_ = Mode::kIdle;
  StopReason reason_ = StopReason::kNone;
  base::TimeTicks started_;
  base::TimeDelta elapsed_;
  size_t output_bytes_ = 0;
  bool cleanup_exhausted_ = false;
  unsigned trace_events_ = 0;
  struct EventUsage {
    base::TimeTicks finished;
    base::TimeDelta time;
    size_t dom_calls = 0;
    size_t output_bytes = 0;
    base::TimeDelta frame_time;
  };
  void ExpireEvents();
  std::deque<EventUsage> events_;
  base::TimeDelta event_time_;
  base::TimeDelta frame_time_;
  size_t event_dom_calls_ = 0;
  size_t event_output_bytes_ = 0;
  size_t initialization_dom_calls_ = 0;
  size_t callback_dom_calls_ = 0;
  size_t callback_output_bytes_ = 0;
  bool IsCallback() const { return kind_ != ExecutionKind::kInitialization; }
  ExecutionKind kind_ = ExecutionKind::kInitialization;
  ExecutionKind stopped_kind_ = ExecutionKind::kInitialization;
};

// SetTrace holds a capsule pointing to a context-owned ExecutionBudget. The
// owner remains alive throughout Py_EndInterpreter, including trace cleanup.
int BudgetTrace(PyObject* token,
                PyFrameObject* frame,
                int event,
                PyObject* arg);

class BudgetedDomHost final : public DomHost {
 public:
  explicit BudgetedDomHost(ExecutionBudget& budget);
  bool IsActive() const override;
  bool CanBuildCanvas() const override;
  DomResult GetElementById(std::string_view id) override;
  DomResult SetText(DomHandle handle, std::string_view text) override;
  ControlStateResult GetControlState(DomHandle) override;
  DomResult UpdateElement(DomHandle, const ElementUpdate&) override;
  DomResult AddEventListener(DomHandle,
                             std::string_view,
                             CallbackHandle,
                             bool once,
                             bool passive,
                             bool capture) override;
  DomResult RemoveEventListener(DomHandle, CallbackHandle) override;
  void RevokeEventListener(DomHandle, CallbackHandle) override;
  DomResult RequestFrame(CallbackHandle) override;
  DomResult CancelFrame(CallbackHandle) override;
  void RevokeFrame(CallbackHandle) override;
  DomResult GetEventTarget(TargetKind) override;
  PageStateResult GetPageState() override;
  DomResult ControlEvent(uint64_t, EventAction) override;
  DomResult SetPointerCapture(DomHandle, int32_t, bool) override;

  CanvasResult GetCanvasContext(DomHandle) override;
  CanvasResult CreateCanvasSurface(uint32_t, uint32_t) override;
  CanvasResult ResizeCanvas(CanvasHandle, uint32_t, uint32_t) override;
  DomResult ReleaseCanvas(CanvasHandle) override;
  CanvasResult SubmitCanvas(CanvasHandle,
                            const std::vector<CanvasCommand>&) override;

 private:
  DomResult FinishCall(DomResult result);
  const raw_ref<ExecutionBudget> budget_;
};

inline constexpr char kBudgetCapsuleName[] = "native_python.ExecutionBudget";

}  // namespace native_python

#endif  // COMPONENTS_PYTHON_RUNTIME_PYTHON_EXECUTION_BUDGET_H_
