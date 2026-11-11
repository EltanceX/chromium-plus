// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/python_document_context.h"

#include "base/memory/raw_ref.h"
#include "components/python_runtime/python_runtime.h"

namespace {
static_assert(static_cast<int>(native_python::CanvasOp::kClearRect) ==
              static_cast<int>(blink::WebPythonCanvasOp::kClearRect));
static_assert(static_cast<int>(native_python::CanvasOp::kFillRect) ==
              static_cast<int>(blink::WebPythonCanvasOp::kFillRect));
static_assert(static_cast<int>(native_python::CanvasOp::kStrokeRect) ==
              static_cast<int>(blink::WebPythonCanvasOp::kStrokeRect));
static_assert(static_cast<int>(native_python::CanvasOp::kBeginPath) ==
              static_cast<int>(blink::WebPythonCanvasOp::kBeginPath));
static_assert(static_cast<int>(native_python::CanvasOp::kClosePath) ==
              static_cast<int>(blink::WebPythonCanvasOp::kClosePath));
static_assert(static_cast<int>(native_python::CanvasOp::kMoveTo) ==
              static_cast<int>(blink::WebPythonCanvasOp::kMoveTo));
static_assert(static_cast<int>(native_python::CanvasOp::kLineTo) ==
              static_cast<int>(blink::WebPythonCanvasOp::kLineTo));
static_assert(static_cast<int>(native_python::CanvasOp::kArc) ==
              static_cast<int>(blink::WebPythonCanvasOp::kArc));
static_assert(static_cast<int>(native_python::CanvasOp::kEllipse) ==
              static_cast<int>(blink::WebPythonCanvasOp::kEllipse));
static_assert(static_cast<int>(native_python::CanvasOp::kFill) ==
              static_cast<int>(blink::WebPythonCanvasOp::kFill));
static_assert(static_cast<int>(native_python::CanvasOp::kStroke) ==
              static_cast<int>(blink::WebPythonCanvasOp::kStroke));
static_assert(static_cast<int>(native_python::CanvasOp::kSave) ==
              static_cast<int>(blink::WebPythonCanvasOp::kSave));
static_assert(static_cast<int>(native_python::CanvasOp::kRestore) ==
              static_cast<int>(blink::WebPythonCanvasOp::kRestore));
static_assert(static_cast<int>(native_python::CanvasOp::kTranslate) ==
              static_cast<int>(blink::WebPythonCanvasOp::kTranslate));
static_assert(static_cast<int>(native_python::CanvasOp::kScale) ==
              static_cast<int>(blink::WebPythonCanvasOp::kScale));
static_assert(static_cast<int>(native_python::CanvasOp::kRotate) ==
              static_cast<int>(blink::WebPythonCanvasOp::kRotate));
static_assert(static_cast<int>(native_python::CanvasOp::kFillColor) ==
              static_cast<int>(blink::WebPythonCanvasOp::kFillColor));
static_assert(static_cast<int>(native_python::CanvasOp::kStrokeColor) ==
              static_cast<int>(blink::WebPythonCanvasOp::kStrokeColor));
static_assert(static_cast<int>(native_python::CanvasOp::kFillGradient) ==
              static_cast<int>(blink::WebPythonCanvasOp::kFillGradient));
static_assert(static_cast<int>(native_python::CanvasOp::kStrokeGradient) ==
              static_cast<int>(blink::WebPythonCanvasOp::kStrokeGradient));
static_assert(static_cast<int>(native_python::CanvasOp::kLinearGradient) ==
              static_cast<int>(blink::WebPythonCanvasOp::kLinearGradient));
static_assert(static_cast<int>(native_python::CanvasOp::kColorStop) ==
              static_cast<int>(blink::WebPythonCanvasOp::kColorStop));
static_assert(static_cast<int>(native_python::CanvasOp::kLineWidth) ==
              static_cast<int>(blink::WebPythonCanvasOp::kLineWidth));
static_assert(static_cast<int>(native_python::CanvasOp::kLineDash) ==
              static_cast<int>(blink::WebPythonCanvasOp::kLineDash));
static_assert(static_cast<int>(native_python::CanvasOp::kAlpha) ==
              static_cast<int>(blink::WebPythonCanvasOp::kAlpha));
static_assert(static_cast<int>(native_python::CanvasOp::kFont) ==
              static_cast<int>(blink::WebPythonCanvasOp::kFont));
static_assert(static_cast<int>(native_python::CanvasOp::kTextAlign) ==
              static_cast<int>(blink::WebPythonCanvasOp::kTextAlign));
static_assert(static_cast<int>(native_python::CanvasOp::kTextBaseline) ==
              static_cast<int>(blink::WebPythonCanvasOp::kTextBaseline));
static_assert(static_cast<int>(native_python::CanvasOp::kFillText) ==
              static_cast<int>(blink::WebPythonCanvasOp::kFillText));
static_assert(static_cast<int>(native_python::CanvasOp::kSmoothing) ==
              static_cast<int>(blink::WebPythonCanvasOp::kSmoothing));
static_assert(static_cast<int>(native_python::CanvasOp::kDrawSurface) ==
              static_cast<int>(blink::WebPythonCanvasOp::kDrawSurface));

blink::WebPythonScriptResult ConvertResult(
    const native_python::ExecutionResult& result) {
  return {result.success,
          result.output_truncated,
          blink::WebString::FromUtf8(result.stdout_text),
          blink::WebString::FromUtf8(result.stderr_text),
          blink::WebString::FromUtf8(result.error),
          result.stop_reason != native_python::StopReason::kNone,
          result.callback_skipped};
}

blink::WebPythonDomHandle ToWeb(native_python::DomHandle h) {
  return {h.document, h.generation, h.id,
          static_cast<blink::WebPythonTargetKind>(h.kind)};
}
native_python::DomHandle FromWeb(blink::WebPythonDomHandle h) {
  return {h.document, h.generation, h.id,
          static_cast<native_python::TargetKind>(h.kind)};
}

blink::WebPythonCanvasHandle ToWeb(native_python::CanvasHandle h) {
  return {h.document, h.generation, h.id, h.version};
}
native_python::CanvasHandle FromWeb(blink::WebPythonCanvasHandle h) {
  return {h.document, h.generation, h.id, h.version};
}

class ChromeDomHost final : public native_python::DomHost {
 public:
  explicit ChromeDomHost(blink::WebPythonDomHost& host) : host_(host) {}
  bool IsActive() const override { return host_->IsActive(); }
  native_python::DomResult GetElementById(std::string_view id) override {
    return Convert(host_->GetElementById(blink::WebString::FromUtf8(id)));
  }
  native_python::DomResult SetText(native_python::DomHandle handle,
                                   std::string_view text) override {
    return Convert(
        host_->SetText(ToWeb(handle), blink::WebString::FromUtf8(text)));
  }

  native_python::ControlStateResult GetControlState(
      native_python::DomHandle handle) override {
    const auto r = host_->GetControlState(ToWeb(handle));
    return {Convert(r.status), r.value.Utf8(), r.checked,
            r.hidden,          r.disabled,     r.open};
  }
  native_python::DomResult UpdateElement(
      native_python::DomHandle handle,
      const native_python::ElementUpdate& update) override {
    return Convert(host_->UpdateElement(
        ToWeb(handle),
        {static_cast<blink::WebPythonElementAction>(update.action),
         blink::WebString::FromUtf8(update.name),
         blink::WebString::FromUtf8(update.value), update.enabled,
         update.number}));
  }

  native_python::DomResult AddEventListener(
      native_python::DomHandle element,
      std::string_view type,
      native_python::CallbackHandle callback,
      bool once,
      bool passive,
      bool capture) override {
    return Convert(host_->AddEventListener(
        ToWeb(element), blink::WebString::FromUtf8(type),
        {callback.context, callback.id}, once, passive, capture));
  }
  native_python::DomResult RemoveEventListener(
      native_python::DomHandle element,
      native_python::CallbackHandle callback) override {
    return Convert(host_->RemoveEventListener(ToWeb(element),
                                              {callback.context, callback.id}));
  }
  void RevokeEventListener(native_python::DomHandle element,
                           native_python::CallbackHandle callback) override {
    host_->RevokeEventListener(ToWeb(element), {callback.context, callback.id});
  }
  native_python::DomResult RequestFrame(
      native_python::CallbackHandle callback) override {
    return Convert(host_->RequestFrame({callback.context, callback.id}));
  }
  native_python::DomResult CancelFrame(
      native_python::CallbackHandle callback) override {
    return Convert(host_->CancelFrame({callback.context, callback.id}));
  }
  void RevokeFrame(native_python::CallbackHandle callback) override {
    host_->RevokeFrame({callback.context, callback.id});
  }

  native_python::DomResult GetEventTarget(
      native_python::TargetKind kind) override {
    return Convert(
        host_->GetEventTarget(static_cast<blink::WebPythonTargetKind>(kind)));
  }
  native_python::PageStateResult GetPageState() override {
    const auto r = host_->GetPageState();
    return {Convert(r.status),
            {r.state.hidden, r.state.has_focus, r.state.width, r.state.height,
             r.state.reduced_motion, r.state.input_epoch}};
  }
  native_python::DomResult ControlEvent(
      uint64_t token,
      native_python::EventAction action) override {
    return Convert(host_->ControlEvent(
        token, static_cast<blink::WebPythonEventAction>(action)));
  }
  native_python::DomResult SetPointerCapture(native_python::DomHandle h,
                                             int32_t id,
                                             bool capture) override {
    return Convert(host_->SetPointerCapture(ToWeb(h), id, capture));
  }

  native_python::CanvasResult GetCanvasContext(
      native_python::DomHandle h) override {
    return ConvertCanvas(host_->GetCanvasContext(ToWeb(h)));
  }
  native_python::CanvasResult CreateCanvasSurface(uint32_t w,
                                                  uint32_t h) override {
    return ConvertCanvas(host_->CreateCanvasSurface(w, h));
  }
  native_python::CanvasResult ResizeCanvas(native_python::CanvasHandle c,
                                           uint32_t w,
                                           uint32_t h) override {
    return ConvertCanvas(host_->ResizeCanvas(ToWeb(c), w, h));
  }
  native_python::DomResult ReleaseCanvas(
      native_python::CanvasHandle c) override {
    return Convert(host_->ReleaseCanvas(ToWeb(c)));
  }
  native_python::CanvasResult SubmitCanvas(
      native_python::CanvasHandle c,
      const std::vector<native_python::CanvasCommand>& commands) override {
    std::vector<blink::WebPythonCanvasCommand> converted;
    converted.reserve(commands.size());
    for (const auto& command : commands) {
      converted.push_back({static_cast<blink::WebPythonCanvasOp>(command.op),
                           command.args, command.text, command.values,
                           ToWeb(command.source), command.index});
    }
    return ConvertCanvas(host_->SubmitCanvas(ToWeb(c), converted));
  }

 private:
  static native_python::CanvasResult ConvertCanvas(
      const blink::WebPythonCanvasResult& r) {
    return {Convert(r.status),
            FromWeb(r.handle),
            {r.statistics.commands, r.statistics.bytes, r.statistics.pixels,
             r.statistics.native_ms}};
  }
  static native_python::DomResult Convert(
      const blink::WebPythonDomResult& result) {
    native_python::DomError error;
    switch (result.error) {
      case blink::WebPythonDomError::kNone:
        error = native_python::DomError::kNone;
        break;
      case blink::WebPythonDomError::kReference:
        error = native_python::DomError::kReference;
        break;
      case blink::WebPythonDomError::kNotAllowed:
        error = native_python::DomError::kNotAllowed;
        break;
      case blink::WebPythonDomError::kLimit:
        error = native_python::DomError::kLimit;
        break;
      case blink::WebPythonDomError::kState:
        error = native_python::DomError::kState;
        break;
      case blink::WebPythonDomError::kDrawingLimit:
        error = native_python::DomError::kDrawingLimit;
        break;
      case blink::WebPythonDomError::kStopped:
        error = native_python::DomError::kStopped;
        break;
    }
    return {FromWeb(result.handle), error, result.message.Utf8()};
  }
  const raw_ref<blink::WebPythonDomHost> host_;
};

class ChromePythonDocumentContext final : public blink::WebPythonScriptContext {
 public:
  ChromePythonDocumentContext()
      : context_(native_python::PythonContext::Create(&error_)) {}

  std::vector<blink::WebPythonScriptResult> Run(
      const std::vector<blink::WebPythonScriptSource>& sources,
      blink::WebPythonDomHost& host,
      ReportResult report_result) override {
    ChromeDomHost bridge(host);
    std::vector<blink::WebPythonScriptResult> results;
    for (const auto& source : sources) {
      native_python::ExecutionResult result;
      if (!host.IsActive()) {
        result.error = "The Python document context is inactive";
        result.stop_reason = native_python::StopReason::kInactive;
      } else if (context_) {
        result = context_->Execute(source.source.Utf8(), source.filename.Utf8(),
                                   &bridge);
      } else {
        result.error = error_;
        result.stop_reason = native_python::StopReason::kInactive;
      }
      results.push_back(ConvertResult(result));
      const bool keep_going = report_result(results.back());
      if (results.back().stopped || !keep_going) {
        break;
      }
    }
    return results;
  }

  blink::WebPythonScriptResult InvokeCallback(
      blink::WebPythonCallbackHandle handle,
      const blink::WebPythonCallbackEvent& event,
      blink::WebPythonDomHost& host) override {
    if (!context_ || !host.IsActive()) {
      return ConvertResult(InactiveResult());
    }
    ChromeDomHost bridge(host);
    native_python::CallbackEvent snapshot;
    snapshot.current_target = FromWeb(event.current_target);
    snapshot.type = event.type.Utf8();
    snapshot.is_trusted = event.is_trusted;
    snapshot.control_token = event.control_token;
    snapshot.cancelable = event.cancelable;
    snapshot.bubbles = event.bubbles;
    snapshot.default_prevented = event.default_prevented;
    snapshot.passive = event.passive;
    snapshot.target_is_editable = event.target_is_editable;
    snapshot.target_in_dialog = event.target_in_dialog;
    snapshot.key = event.key.Utf8();
    snapshot.code = event.code.Utf8();
    snapshot.repeat = event.repeat;
    snapshot.is_composing = event.is_composing;
    snapshot.alt_key = event.alt_key;
    snapshot.ctrl_key = event.ctrl_key;
    snapshot.meta_key = event.meta_key;
    snapshot.shift_key = event.shift_key;
    snapshot.pointer_id = event.pointer_id;
    snapshot.pointer_type = event.pointer_type.Utf8();
    snapshot.button = event.button;
    snapshot.buttons = event.buttons;
    snapshot.client_x = event.client_x;
    snapshot.client_y = event.client_y;
    snapshot.is_primary = event.is_primary;
    snapshot.value = event.value.Utf8();
    snapshot.checked = event.checked;
    snapshot.value_truncated = event.value_truncated;
    snapshot.input_epoch = event.input_epoch;
    return ConvertResult(context_->InvokeCallback({handle.context, handle.id},
                                                  snapshot, &bridge));
  }

  bool CanInvokeCallback() override {
    return context_ && context_->CanInvokeCallback();
  }

  bool CanInvokeFrameCallback() override {
    return context_ && context_->CanInvokeFrameCallback();
  }

  blink::WebPythonScriptResult InvokeFrameCallback(
      blink::WebPythonCallbackHandle handle,
      double timestamp_ms,
      blink::WebPythonDomHost& host) override {
    if (!context_ || !host.IsActive()) {
      return ConvertResult(InactiveResult());
    }
    ChromeDomHost bridge(host);
    return ConvertResult(context_->InvokeFrameCallback(
        {handle.context, handle.id}, timestamp_ms, &bridge));
  }

  blink::WebPythonScriptResult RemoveCallback(
      blink::WebPythonCallbackHandle handle) override {
    return ConvertResult(
        context_ ? context_->RemoveCallback({handle.context, handle.id})
                 : InactiveResult());
  }

 private:
  native_python::ExecutionResult InactiveResult() const {
    native_python::ExecutionResult result;
    result.error =
        context_ ? "The Python document context is inactive" : error_;
    result.stop_reason = native_python::StopReason::kInactive;
    return result;
  }
  std::string error_;
  std::unique_ptr<native_python::PythonContext> context_;
};

}  // namespace

std::unique_ptr<blink::WebPythonScriptContext> CreatePythonDocumentContext() {
  return std::make_unique<ChromePythonDocumentContext>();
}
