// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/python_script_runner.h"

#include "base/command_line.h"
#include "base/no_destructor.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/native_python_policy.h"
#include "chrome/renderer/python_document_context.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/web/web_python_script_runner.h"

namespace {

class ChromePythonScriptRunner final : public blink::WebPythonScriptRunner {
 public:
  ChromePythonScriptRunner()
      : allowed_origin_(ParseNativePythonAllowedOrigin(
            base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
                switches::kNativePythonAllowedOrigin))),
        unrestricted_pages_(base::CommandLine::ForCurrentProcess()->HasSwitch(
            kNativePythonUnrestrictedPages)) {}

  bool IsOriginAllowed(const blink::WebSecurityOrigin& origin) const override {
    return allowed_origin_ && !origin.IsOpaque() &&
           static_cast<url::Origin>(origin) == *allowed_origin_;
  }

  bool AllowsUnrestrictedPages() const override {
    // Renderer switches are immutable. This is also used by the cooperative
    // trace hook and every native Canvas command; avoid reparsing the switch.
    return unrestricted_pages_;
  }

  std::unique_ptr<blink::WebPythonScriptContext> CreateContext() override {
    return CreatePythonDocumentContext();
  }

 private:
  const std::optional<url::Origin> allowed_origin_;
  const bool unrestricted_pages_;
};

}  // namespace

void InitializePythonScriptRunner() {
  static base::NoDestructor<ChromePythonScriptRunner> runner;
  blink::WebPythonScriptRunner::SetForProcess(runner.get());
}
