// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/python_script_runner.h"

#include "base/command_line.h"
#include "base/no_destructor.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/native_python_policy.h"
#include "components/python_runtime/python_runtime.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/web/web_python_script_runner.h"

namespace {

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

  std::vector<blink::WebPythonScriptResult> Run(
      const std::vector<blink::WebPythonScriptSource>& sources) override {
    std::string error;
    auto context = native_python::PythonContext::Create(&error);
    std::vector<blink::WebPythonScriptResult> results;
    for (const auto& source : sources) {
      native_python::ExecutionResult result;
      if (context) {
        result = context->Execute(source.source.Utf8(), source.filename.Utf8());
      } else {
        result.error = error;
      }
      results.push_back({result.success, result.output_truncated,
                         blink::WebString::FromUtf8(result.stdout_text),
                         blink::WebString::FromUtf8(result.stderr_text),
                         blink::WebString::FromUtf8(result.error)});
    }
    // In phase two no Python state survives parsing completion. Phase three
    // will introduce a document-owned context alongside the DOM bridge.
    return results;
  }

 private:
  const std::optional<url::Origin> allowed_origin_;
};

}  // namespace

void InitializePythonScriptRunner() {
  static base::NoDestructor<ChromePythonScriptRunner> runner;
  blink::WebPythonScriptRunner::SetForProcess(runner.get());
}
