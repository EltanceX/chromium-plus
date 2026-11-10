// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/python_runtime/python_execution_budget.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/time/tick_clock.h"

namespace native_python {
namespace {
constexpr size_t kOutputLimit = 16 * 1024;
constexpr base::TimeDelta kBlockTime = base::Milliseconds(100);
constexpr base::TimeDelta kDocumentTime = base::Milliseconds(250);
constexpr base::TimeDelta kCleanupTime = base::Milliseconds(50);
constexpr base::TimeDelta kCallbackTime = base::Milliseconds(50);
constexpr base::TimeDelta kFrameTime = base::Milliseconds(8);
constexpr base::TimeDelta kFrameWindowTime = base::Milliseconds(200);
constexpr base::TimeDelta kEventWindow = base::Seconds(1);
constexpr size_t kCallbackDomCalls = 128;
constexpr size_t kDomCalls = 1024;
constexpr size_t kCallbackOutput = 4 * 1024;
}  // namespace

ExecutionBudget::ExecutionBudget(const base::TickClock* clock)
    : clock_(*clock) {}

ExecutionBudget::~ExecutionBudget() = default;

void ExecutionBudget::ExpireEvents() {
  const auto now = clock_->NowTicks();
  while (!events_.empty() && now - events_.front().finished >= kEventWindow) {
    event_time_ -= events_.front().time;
    frame_time_ -= events_.front().frame_time;
    event_dom_calls_ -= events_.front().dom_calls;
    event_output_bytes_ -= events_.front().output_bytes;
    events_.pop_front();
  }
}

bool ExecutionBudget::CanBeginCallback(ExecutionKind kind) {
  ExpireEvents();
  const bool frame = kind == ExecutionKind::kAnimationFrame;
  return mode_ == Mode::kIdle && reason_ == StopReason::kNone &&
         kind != ExecutionKind::kInitialization &&
         event_time_ <= kDocumentTime - (frame ? kFrameTime : kCallbackTime) &&
         (!frame || frame_time_ <= kFrameWindowTime - kFrameTime) &&
         event_dom_calls_ <= kDomCalls - kCallbackDomCalls &&
         event_output_bytes_ <= kOutputLimit - kCallbackOutput;
}

bool ExecutionBudget::Begin(DomHost* host, ExecutionKind kind) {
  CHECK(mode_ == Mode::kIdle);
  if (kind != ExecutionKind::kInitialization && !CanBeginCallback(kind)) {
    return false;
  }
  kind_ = kind;
  callback_dom_calls_ = 0;
  callback_output_bytes_ = 0;
  host_ = host;
  started_ = clock_->NowTicks();
  mode_ = Mode::kExecute;
  trace_events_ = 0;
  return true;
}

void ExecutionBudget::Finish() {
  if (mode_ == Mode::kExecute) {
    Check();
    const auto now = clock_->NowTicks();
    if (IsCallback()) {
      const EventUsage usage{
          now, now - started_, callback_dom_calls_, callback_output_bytes_,
          kind_ == ExecutionKind::kAnimationFrame ? now - started_
                                                  : base::TimeDelta()};
      event_time_ += usage.time;
      frame_time_ += usage.frame_time;
      event_dom_calls_ += usage.dom_calls;
      event_output_bytes_ += usage.output_bytes;
      // Bound bookkeeping even for many tiny callbacks. Coalescing retains
      // older charges until the newest completion expires, conservatively.
      if (events_.size() == 1024) {
        auto& last = events_.back();
        last.finished = now;
        last.time += usage.time;
        last.frame_time += usage.frame_time;
        last.dom_calls += usage.dom_calls;
        last.output_bytes += usage.output_bytes;
      } else if (!usage.time.is_zero() || usage.dom_calls ||
                 usage.output_bytes) {
        events_.push_back(usage);
      }
    } else {
      elapsed_ += now - started_;
    }
  }
  host_ = nullptr;
  mode_ = Mode::kIdle;
}

void ExecutionBudget::BeginCleanup() {
  CHECK(mode_ == Mode::kIdle);
  host_ = nullptr;
  mode_ = Mode::kCleanup;
  started_ = clock_->NowTicks();
}

void ExecutionBudget::Stop(StopReason reason) {
  if (reason_ == StopReason::kNone) {
    reason_ = reason;
    stopped_kind_ = kind_;
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
  if (!IsCallback() && elapsed_ + duration >= kDocumentTime) {
    Stop(StopReason::kDocumentTime);
  } else if (duration >= (kind_ == ExecutionKind::kAnimationFrame ? kFrameTime
                          : IsCallback() ? kCallbackTime
                                         : kBlockTime)) {
    Stop(StopReason::kBlockTime);
  }
  return reason_ == StopReason::kNone;
}

const char* ExecutionBudget::Message() const {
  switch (reason_) {
    case StopReason::kNone:
      return "Python execution context is inactive";
    case StopReason::kBlockTime:
      if (stopped_kind_ == ExecutionKind::kAnimationFrame) {
        return "Python animation frame exceeded the 8 ms cooperative execution "
               "budget";
      }
      return stopped_kind_ != ExecutionKind::kInitialization
                 ? "Python callback exceeded the 50 ms "
                   "cooperative execution budget"
                 : "Python block exceeded the 100 ms "
                   "cooperative execution budget";
    case StopReason::kDocumentTime:
      return "Python document exceeded the 250 ms cooperative execution budget";
    case StopReason::kOutput:
      return stopped_kind_ != ExecutionKind::kInitialization
                 ? "Python callback output was truncated at 4 "
                   "KiB; document execution stopped"
                 : "Python output was truncated at 16 KiB; "
                   "document execution stopped";
    case StopReason::kDomCalls:
      return stopped_kind_ != ExecutionKind::kInitialization
                 ? "Python callback exceeded the 128 DOM call budget"
                 : "Python document exceeded the 1024 DOM call budget";
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
  (IsCallback() ? callback_output_bytes_ : output_bytes_) += accepted;
  if (accepted != bytes) {
    Stop(StopReason::kOutput);
    return false;
  }
  return true;
}

size_t ExecutionBudget::OutputRemaining() const {
  return IsCallback() ? kCallbackOutput - callback_output_bytes_
                      : kOutputLimit - output_bytes_;
}

bool ExecutionBudget::ConsumeDomCall() {
  if (!Check()) {
    return false;
  }
  auto& calls = IsCallback() ? callback_dom_calls_ : initialization_dom_calls_;
  if (calls == (IsCallback() ? kCallbackDomCalls : kDomCalls)) {
    Stop(StopReason::kDomCalls);
    return false;
  }
  ++calls;
  return true;
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
  if (!IsActive() || !budget_->ConsumeDomCall()) {
    return {{}, DomError::kStopped, budget_->Message()};
  }
  return FinishCall(budget_->host()->GetElementById(id));
}

DomResult BudgetedDomHost::SetText(DomHandle handle, std::string_view text) {
  if (!IsActive() || !budget_->ConsumeDomCall()) {
    return {{}, DomError::kStopped, budget_->Message()};
  }
  return FinishCall(budget_->host()->SetText(handle, text));
}

DomResult BudgetedDomHost::AddEventListener(DomHandle element,
                                            std::string_view type,
                                            CallbackHandle callback,
                                            bool once,
                                            bool passive,
                                            bool capture) {
  if (!IsActive() || !budget_->ConsumeDomCall()) {
    return {{}, DomError::kStopped, budget_->Message()};
  }
  return FinishCall(budget_->host()->AddEventListener(element, type, callback,
                                                      once, passive, capture));
}

DomResult BudgetedDomHost::RemoveEventListener(DomHandle element,
                                               CallbackHandle callback) {
  if (!IsActive() || !budget_->ConsumeDomCall()) {
    return {{}, DomError::kStopped, budget_->Message()};
  }
  return FinishCall(budget_->host()->RemoveEventListener(element, callback));
}

void BudgetedDomHost::RevokeEventListener(DomHandle element,
                                          CallbackHandle callback) {
  // Rollback must remain possible after the post-call budget/activity check.
  if (budget_->executing() && budget_->host()) {
    budget_->host()->RevokeEventListener(element, callback);
  }
}

DomResult BudgetedDomHost::RequestFrame(CallbackHandle callback) {
  if (!IsActive() || !budget_->ConsumeDomCall()) {
    return {{}, DomError::kStopped, budget_->Message()};
  }
  return FinishCall(budget_->host()->RequestFrame(callback));
}

DomResult BudgetedDomHost::CancelFrame(CallbackHandle callback) {
  if (!IsActive() || !budget_->ConsumeDomCall()) {
    return {{}, DomError::kStopped, budget_->Message()};
  }
  return FinishCall(budget_->host()->CancelFrame(callback));
}

void BudgetedDomHost::RevokeFrame(CallbackHandle callback) {
  if (budget_->executing() && budget_->host()) {
    budget_->host()->RevokeFrame(callback);
  }
}

DomResult BudgetedDomHost::GetEventTarget(TargetKind kind) {
  if (!IsActive() || !budget_->ConsumeDomCall()) {
    return {{}, DomError::kStopped, budget_->Message()};
  }
  return FinishCall(budget_->host()->GetEventTarget(kind));
}

PageStateResult BudgetedDomHost::GetPageState() {
  if (!IsActive() || !budget_->ConsumeDomCall()) {
    return {{{}, DomError::kStopped, budget_->Message()}, {}};
  }
  auto result = budget_->host()->GetPageState();
  result.status = FinishCall(std::move(result.status));
  return result;
}

DomResult BudgetedDomHost::ControlEvent(uint64_t token, EventAction action) {
  if (!IsActive() || !budget_->ConsumeDomCall()) {
    return {{}, DomError::kStopped, budget_->Message()};
  }
  return FinishCall(budget_->host()->ControlEvent(token, action));
}

DomResult BudgetedDomHost::SetPointerCapture(DomHandle target,
                                             int32_t id,
                                             bool capture) {
  if (!IsActive() || !budget_->ConsumeDomCall()) {
    return {{}, DomError::kStopped, budget_->Message()};
  }
  return FinishCall(budget_->host()->SetPointerCapture(target, id, capture));
}

}  // namespace native_python
