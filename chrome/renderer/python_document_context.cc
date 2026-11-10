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
        host_->SetText({handle.document, handle.generation, handle.id},
                       blink::WebString::FromUtf8(text)));
  }

  native_python::DomResult AddEventListener(
      native_python::DomHandle element,
      std::string_view type,
      native_python::CallbackHandle callback,
      bool once) override {
    return Convert(host_->AddEventListener(
        {element.document, element.generation, element.id},
        blink::WebString::FromUtf8(type), {callback.context, callback.id},
        once));
  }
  native_python::DomResult RemoveEventListener(
      native_python::DomHandle element,
      native_python::CallbackHandle callback) override {
    return Convert(host_->RemoveEventListener(
        {element.document, element.generation, element.id},
        {callback.context, callback.id}));
  }
  void RevokeEventListener(native_python::DomHandle element,
                           native_python::CallbackHandle callback) override {
    host_->RevokeEventListener(
        {element.document, element.generation, element.id},
        {callback.context, callback.id});
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
    return {
        {result.handle.document, result.handle.generation, result.handle.id},
        error,
        result.message.Utf8()};
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
    return ConvertResult(context_->InvokeCallback(
        {handle.context, handle.id},
        {event.type.Utf8(),
         {event.current_target.document, event.current_target.generation,
          event.current_target.id},
         event.is_trusted},
        &bridge));
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
