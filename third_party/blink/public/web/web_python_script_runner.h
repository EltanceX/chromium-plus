// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_PYTHON_SCRIPT_RUNNER_H_
#define THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_PYTHON_SCRIPT_RUNNER_H_

#include <vector>

#include "third_party/blink/public/platform/web_common.h"
#include "third_party/blink/public/platform/web_string.h"

namespace blink {

class WebSecurityOrigin;

struct WebPythonScriptSource {
  WebString source;
  WebString filename;
};

struct WebPythonScriptResult {
  bool success = false;
  bool output_truncated = false;
  WebString stdout_text;
  WebString stderr_text;
  WebString error;
};

// Main-thread embedder interface for the private PythonScript experiment.
// Blink checks document eligibility and CSP before passing immutable sources.
// A batch uses one interpreter and must release it before returning. No page
// objects, DOM access, asynchronous callbacks or retained code are exposed
// here.
class BLINK_EXPORT WebPythonScriptRunner {
 public:
  virtual ~WebPythonScriptRunner() = default;
  virtual bool IsOriginAllowed(const WebSecurityOrigin&) const = 0;
  virtual std::vector<WebPythonScriptResult> Run(
      const std::vector<WebPythonScriptSource>&) = 0;

  // Caller owns the runner and must outlive its installation. Main thread only.
  static void SetForProcess(WebPythonScriptRunner*);
  static WebPythonScriptRunner* GetForProcess();
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_PYTHON_SCRIPT_RUNNER_H_
