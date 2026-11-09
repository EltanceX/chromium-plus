// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PYTHON_RUNTIME_PYTHON_EXECUTION_BUDGET_H_
#define COMPONENTS_PYTHON_RUNTIME_PYTHON_EXECUTION_BUDGET_H_

#include <Python.h>

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
  void Begin(DomHost* host);
  void Finish();
  void BeginCleanup();
  bool Check();
  void Stop(StopReason reason);
  bool RaiseIfStopped();
  bool TraceCheckpoint(int event);
  bool ConsumeOutput(size_t bytes);
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
  DomResult GetElementById(std::string_view id) override;
  DomResult SetText(DomHandle handle, std::string_view text) override;
  DomResult AddEventListener(DomHandle,
                             std::string_view,
                             CallbackHandle,
                             bool once) override;
  DomResult RemoveEventListener(DomHandle, CallbackHandle) override;
  void RevokeEventListener(DomHandle, CallbackHandle) override;

 private:
  DomResult FinishCall(DomResult result);
  const raw_ref<ExecutionBudget> budget_;
};

inline constexpr char kBudgetCapsuleName[] = "native_python.ExecutionBudget";

}  // namespace native_python

#endif  // COMPONENTS_PYTHON_RUNTIME_PYTHON_EXECUTION_BUDGET_H_
