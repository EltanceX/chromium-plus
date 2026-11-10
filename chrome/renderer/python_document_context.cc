// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/python_document_context.h"

#include "base/memory/raw_ref.h"
#include "components/python_runtime/python_runtime.h"

namespace {

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

 private:
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
