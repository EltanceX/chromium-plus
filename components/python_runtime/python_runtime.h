// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PYTHON_RUNTIME_PYTHON_RUNTIME_H_
#define COMPONENTS_PYTHON_RUNTIME_PYTHON_RUNTIME_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>

#include "base/feature_list.h"
#include "base/values.h"
#include "components/python_runtime/python_browser_types.h"
#include "components/python_runtime/python_canvas_types.h"

namespace base {
class TickClock;
}

// Follow Chromium's component linkage so non-component builds share the
// caller's allocator for C++ objects and their destruction.
#if defined(COMPONENT_BUILD)
#if defined(IS_PYTHON_RUNTIME_IMPL)
#define PYTHON_RUNTIME_EXPORT __declspec(dllexport)
#else
#define PYTHON_RUNTIME_EXPORT __declspec(dllimport)
#endif
#else
#define PYTHON_RUNTIME_EXPORT
#endif

namespace native_python {

enum class TargetKind { kElement, kDocument, kWindow };
enum class EventAction {
  kPreventDefault,
  kStopPropagation,
  kStopImmediatePropagation
};

struct DomHandle {
  uint64_t document;
  uint64_t generation;
  uint64_t id;
  TargetKind kind = TargetKind::kElement;
};

struct CallbackHandle {
  uint64_t context = 0;
  uint64_t id = 0;
};

enum class ExecutionKind { kInitialization, kCallback, kAnimationFrame };

struct CallbackEvent {
  std::string type = "click";
  DomHandle current_target{};
  bool is_trusted = false;
  uint64_t control_token = 0;
  bool cancelable = false;
  bool bubbles = false;
  bool default_prevented = false;
  bool passive = false;
  bool target_is_editable = false;
  bool target_in_dialog = false;
  std::string key;
  std::string code;
  bool repeat = false;
  bool is_composing = false;
  bool alt_key = false;
  bool ctrl_key = false;
  bool meta_key = false;
  bool shift_key = false;
  int32_t pointer_id = 0;
  std::string pointer_type;
  int16_t button = 0;
  uint16_t buttons = 0;
  double client_x = 0;
  double client_y = 0;
  bool is_primary = false;
  std::string value;
  bool checked = false;
  bool value_truncated = false;
  uint64_t input_epoch = 0;
};

struct PageState {
  bool hidden = false;
  bool has_focus = false;
  int width = 0;
  int height = 0;
  bool reduced_motion = false;
  uint64_t input_epoch = 0;
};

enum class DomError {
  kNone,
  kReference,
  kNotAllowed,
  kLimit,
  kState,
  kStopped,
  kDrawingLimit
};

enum class StopReason {
  kNone,
  kBlockTime,
  kDocumentTime,
  kOutput,
  kDomCalls,
  kInactive,
  kDrawing,
};

struct PYTHON_RUNTIME_EXPORT DomResult {
  DomHandle handle{};
  DomError error = DomError::kNone;
  std::string message;
};

struct PageStateResult {
  DomResult status;
  PageState state;
};

struct CanvasResult {
  DomResult status;
  CanvasHandle handle;
  CanvasStatistics statistics;
};

struct BrowserRequest {
  BrowserOp op;
  DomHandle element{};
  std::string key;
  std::string value;
  std::vector<AudioTone> tones;
};
struct BrowserResult {
  DomResult status;
  std::string value;
  uint64_t id = 0;
  bool flag = false;
};

enum class ElementAction {
  kValue,
  kChecked,
  kHidden,
  kDisabled,
  kClass,
  kAttribute,
  kTabIndex,
  kProgress,
  kFocus,
  kShowModal,
  kCloseDialog,
  kScrollIntoView
};

struct ElementUpdate {
  ElementAction action;
  std::string name;
  std::string value;
  bool enabled = false;
  double number = 0;
};

struct ControlStateResult {
  DomResult status;
  std::string value;
  bool checked = false;
  bool hidden = false;
  bool disabled = false;
  bool open = false;
};

// Borrowed only during synchronous execution. No Python object contains a
// node pointer or retains this host between calls or during finalization.
class PYTHON_RUNTIME_EXPORT DomHost {
 public:
  virtual ~DomHost() = default;
  virtual bool IsActive() const = 0;
  virtual BrowserResult Browser(const BrowserRequest&);
  // Pure command data has no DOM effects. Embedders may defer full page policy
  // checks to submit, but must retain execution/time/stop checks while
  // building.
  virtual bool CanBuildCanvas() const;
  virtual DomResult GetElementById(std::string_view id) = 0;
  virtual DomResult SetText(DomHandle, std::string_view text) = 0;
  virtual ControlStateResult GetControlState(DomHandle);
  virtual DomResult UpdateElement(DomHandle, const ElementUpdate&);
  virtual DomResult AddEventListener(DomHandle,
                                     std::string_view type,
                                     CallbackHandle,
                                     bool once,
                                     bool passive = false,
                                     bool capture = false) = 0;
  virtual DomResult RemoveEventListener(DomHandle, CallbackHandle) = 0;
  // Idempotent rollback of a possibly installed listener. No policy/budget
  // admission, script entry or Python calls; never exposed as a Python API.
  virtual void RevokeEventListener(DomHandle, CallbackHandle) = 0;
  virtual DomResult RequestFrame(CallbackHandle);
  virtual DomResult CancelFrame(CallbackHandle);
  virtual void RevokeFrame(CallbackHandle) {}
  virtual DomResult GetEventTarget(TargetKind);
  virtual PageStateResult GetPageState();
  virtual DomResult ControlEvent(uint64_t token, EventAction);
  virtual DomResult SetPointerCapture(DomHandle,
                                      int32_t pointer_id,
                                      bool capture);
  virtual CanvasResult GetCanvasContext(DomHandle);
  virtual CanvasResult CreateCanvasSurface(uint32_t width, uint32_t height);
  virtual CanvasResult ResizeCanvas(CanvasHandle,
                                    uint32_t width,
                                    uint32_t height);
  virtual DomResult ReleaseCanvas(CanvasHandle);
  virtual CanvasResult SubmitCanvas(CanvasHandle,
                                    const std::vector<CanvasCommand>&);
};

PYTHON_RUNTIME_EXPORT
BASE_DECLARE_FEATURE(kNativePythonRuntimeSelfTest);

struct PYTHON_RUNTIME_EXPORT ExecutionResult {
  bool success = false;
  bool output_truncated = false;
  std::string stdout_text;
  std::string stderr_text;
  std::string error;
  StopReason stop_reason = StopReason::kNone;
  // Window pressure skips an event without entering Python or consuming once.
  bool callback_skipped = false;
};

struct PYTHON_RUNTIME_EXPORT CallbackRegistrationResult {
  CallbackHandle handle;
  ExecutionResult result;
};

// A single, main-thread-owned subinterpreter. The embedder must check page
// execution policy before use. All instances and the process runtime must use
// the same thread.
class PYTHON_RUNTIME_EXPORT PythonContext {
 public:
  static std::unique_ptr<PythonContext> Create(std::string* error);
  // The clock must outlive the context. Production budgets still apply.
  static std::unique_ptr<PythonContext> CreateForTesting(
      std::string* error,
      const base::TickClock* clock);
  ~PythonContext();
  PythonContext(const PythonContext&) = delete;
  PythonContext& operator=(const PythonContext&) = delete;
  ExecutionResult Execute(std::string_view source,
                          std::string_view filename = "<native-python>",
                          DomHost* dom_host = nullptr);
  // Native registration captures the current value of one globals entry, not
  // source text. Python-facing listener registration is implemented separately.
  CallbackRegistrationResult RegisterCallback(
      std::string_view global_name,
      ExecutionKind kind = ExecutionKind::kCallback);
  ExecutionResult InvokeCallback(CallbackHandle,
                                 const CallbackEvent&,
                                 DomHost* dom_host = nullptr);
  // Main-thread admission probe. With no intervening Python entry, elapsed
  // idle time can only free window capacity, never invalidate admission.
  bool CanInvokeCallback();
  ExecutionResult InvokeFrameCallback(CallbackHandle,
                                      double timestamp_ms,
                                      DomHost* dom_host = nullptr);
  bool CanInvokeFrameCallback();
  // Idempotent for a removed handle from this context. Also permits registry
  // mutation from this context's active host call, without interpreter reentry.
  ExecutionResult RemoveCallback(CallbackHandle);

 private:
  struct Impl;
  static std::unique_ptr<PythonContext> CreateWithClock(
      std::string* error,
      const base::TickClock* clock);
  explicit PythonContext(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

struct RuntimeStatistics {
  size_t contexts_created = 0;
  size_t contexts_destroyed = 0;
  size_t live_contexts = 0;
  size_t cleanup_budget_exhaustions = 0;
};

PYTHON_RUNTIME_EXPORT RuntimeStatistics GetRuntimeStatisticsForTesting();

PYTHON_RUNTIME_EXPORT bool IsInitialized();
PYTHON_RUNTIME_EXPORT std::string Version();
// Fixed diagnostic code only; result can be logged after sandbox engagement.
PYTHON_RUNTIME_EXPORT base::DictValue RunSelfTest();

}  // namespace native_python

#endif  // COMPONENTS_PYTHON_RUNTIME_PYTHON_RUNTIME_H_
