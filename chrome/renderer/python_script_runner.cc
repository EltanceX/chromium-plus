// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/python_script_runner.h"

#include "base/command_line.h"
#include "base/memory/raw_ref.h"
#include "base/no_destructor.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/native_python_policy.h"
#include "components/python_runtime/python_runtime.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/web/web_python_script_runner.h"

namespace {

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
      const bool stopped =
          result.stop_reason != native_python::StopReason::kNone;
      results.push_back({result.success, result.output_truncated,
                         blink::WebString::FromUtf8(result.stdout_text),
                         blink::WebString::FromUtf8(result.stderr_text),
                         blink::WebString::FromUtf8(result.error), stopped});
      const bool keep_going = report_result(results.back());
      if (stopped || !keep_going) {
        break;
      }
    }
    return results;
  }

 private:
  std::string error_;
  std::unique_ptr<native_python::PythonContext> context_;
};

class ChromePythonScriptRunner final : public blink::WebPythonScriptRunner {
 public:
  ChromePythonScriptRunner()
      : allowed_origin_(ParseNativePythonAllowedOrigin(
            base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
                switches::kNativePythonAllowedOrigin))) {}

  bool IsOriginAllowed(const blink::WebSecurityOrigin& origin) const override {
    return allowed_origin_ && !origin.IsOpaque() &&
           static_cast<url::Origin>(origin) == *allowed_origin_;
  }

  std::unique_ptr<blink::WebPythonScriptContext> CreateContext() override {
    return std::make_unique<ChromePythonDocumentContext>();
  }

 private:
  const std::optional<url::Origin> allowed_origin_;
};

}  // namespace

void InitializePythonScriptRunner() {
  static base::NoDestructor<ChromePythonScriptRunner> runner;
  blink::WebPythonScriptRunner::SetForProcess(runner.get());
}
