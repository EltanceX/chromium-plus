// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/python_runtime/python_execution_budget.h"

#include <algorithm>

#include "base/check.h"
#include "base/time/tick_clock.h"

namespace native_python {
namespace {
constexpr size_t kOutputLimit = 16 * 1024;
constexpr base::TimeDelta kBlockTime = base::Milliseconds(100);
constexpr base::TimeDelta kDocumentTime = base::Milliseconds(250);
constexpr base::TimeDelta kCleanupTime = base::Milliseconds(50);
}  // namespace

ExecutionBudget::ExecutionBudget(const base::TickClock* clock)
    : clock_(*clock) {}

void ExecutionBudget::Begin(DomHost* host) {
  CHECK(mode_ == Mode::kIdle);
  host_ = host;
  started_ = clock_->NowTicks();
  mode_ = Mode::kExecute;
  trace_events_ = 0;
}

void ExecutionBudget::Finish() {
  if (mode_ == Mode::kExecute) {
    Check();
    elapsed_ += clock_->NowTicks() - started_;
  }
  host_ = nullptr;
  mode_ = Mode::kIdle;
}

void ExecutionBudget::BeginCleanup() {
  host_ = nullptr;
  mode_ = Mode::kCleanup;
  started_ = clock_->NowTicks();
}

void ExecutionBudget::Stop(StopReason reason) {
  if (reason_ == StopReason::kNone) {
    reason_ = reason;
  }
}

bool ExecutionBudget::Check() {
  if (mode_ == Mode::kIdle) {
    return false;
  }
  const auto duration = clock_->NowTicks() - started_;
  if (mode_ == Mode::kCleanup) {
    cleanup_exhausted_ |= duration >= kCleanupTime;
    return !cleanup_exhausted_;
  }
  if (host_ && !host_->IsActive()) {
    Stop(StopReason::kInactive);
  }
  if (elapsed_ + duration >= kDocumentTime) {
    Stop(StopReason::kDocumentTime);
  } else if (duration >= kBlockTime) {
    Stop(StopReason::kBlockTime);
  }
  return reason_ == StopReason::kNone;
}

const char* ExecutionBudget::Message() const {
  switch (reason_) {
    case StopReason::kNone:
      return "Python execution context is inactive";
    case StopReason::kBlockTime:
      return "Python block exceeded the 100 ms cooperative execution budget";
    case StopReason::kDocumentTime:
      return "Python document exceeded the 250 ms cooperative execution budget";
    case StopReason::kOutput:
      return "Python output was truncated at 16 KiB; document execution "
             "stopped";
    case StopReason::kDomCalls:
      return "Python document exceeded the 1024 DOM call budget";
    case StopReason::kInactive:
      return "The Python document context is inactive";
  }
}

bool ExecutionBudget::RaiseIfStopped() {
  if (Check()) {
    return false;
  }
  const bool timeout = mode_ == Mode::kCleanup ||
                       reason_ == StopReason::kBlockTime ||
                       reason_ == StopReason::kDocumentTime;
  PyErr_SetString(timeout ? PyExc_TimeoutError : PyExc_RuntimeError,
                  mode_ == Mode::kCleanup
                      ? "Python cleanup exceeded its 50 ms cooperative budget"
                      : Message());
  return true;
}

bool ExecutionBudget::TraceCheckpoint(int event) {
  // Bound overhead in tight loops; bridges and output always check separately.
  return (event == PyTrace_CALL || event == PyTrace_RETURN ||
          (++trace_events_ & 63) == 0) &&
         RaiseIfStopped();
}

bool ExecutionBudget::ConsumeOutput(size_t bytes) {
  const size_t accepted = std::min(bytes, OutputRemaining());
  output_bytes_ += accepted;
  if (accepted != bytes) {
    Stop(StopReason::kOutput);
    return false;
  }
  return true;
}

size_t ExecutionBudget::OutputRemaining() const {
  return kOutputLimit - output_bytes_;
}

int BudgetTrace(PyObject* token, PyFrameObject* frame, int event, PyObject*) {
  auto* budget = static_cast<ExecutionBudget*>(
      PyCapsule_GetPointer(token, kBudgetCapsuleName));
  if (!budget || budget->TraceCheckpoint(event)) {
    return -1;
  }
  if (event == PyTrace_CALL &&
      PyObject_SetAttrString(reinterpret_cast<PyObject*>(frame),
                             "f_trace_opcodes", Py_True) < 0) {
    return -1;
  }
  return 0;
}

BudgetedDomHost::BudgetedDomHost(ExecutionBudget& budget) : budget_(budget) {}

bool BudgetedDomHost::IsActive() const {
  return budget_->executing() && budget_->Check() && budget_->host();
}

DomResult BudgetedDomHost::FinishCall(DomResult result) {
  if (result.error == DomError::kStopped) {
    budget_->Stop(StopReason::kDomCalls);
  }
  if (!budget_->Check()) {
    return {{}, DomError::kStopped, budget_->Message()};
  }
  return result;
}

DomResult BudgetedDomHost::GetElementById(std::string_view id) {
  if (!IsActive()) {
    return {{}, DomError::kStopped, budget_->Message()};
  }
  return FinishCall(budget_->host()->GetElementById(id));
}

DomResult BudgetedDomHost::SetText(DomHandle handle, std::string_view text) {
  if (!IsActive()) {
    return {{}, DomError::kStopped, budget_->Message()};
  }
  return FinishCall(budget_->host()->SetText(handle, text));
}

}  // namespace native_python
