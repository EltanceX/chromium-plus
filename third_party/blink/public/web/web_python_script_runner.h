// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_PYTHON_SCRIPT_RUNNER_H_
#define THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_PYTHON_SCRIPT_RUNNER_H_

#include <stdint.h>

#include <memory>
#include <vector>

#include "base/functional/function_ref.h"
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
  // A fatal native stop. Run may return a prefix ending in this result;
  // subsequent sources must not execute in this document.
  bool stopped = false;
};

struct WebPythonDomHandle {
  uint64_t document = 0;
  uint64_t generation = 0;
  uint64_t id = 0;
};

enum class WebPythonDomError {
  kNone,
  kReference,
  kNotAllowed,
  kLimit,
  kState,
  kStopped
};

struct WebPythonDomResult {
  WebPythonDomHandle handle;
  WebPythonDomError error = WebPythonDomError::kNone;
  WebString message;
};

// Borrowed during Run only. Values crossing this boundary never contain Nodes.
class WebPythonDomHost {
 public:
  virtual ~WebPythonDomHost() = default;
  virtual bool IsActive() const = 0;
  virtual WebPythonDomResult GetElementById(const WebString&) = 0;
  virtual WebPythonDomResult SetText(WebPythonDomHandle, const WebString&) = 0;
};

// Owned by a Document controller. Destruction can execute Python finalizers and
// must happen on the main thread outside GC, script-forbidden scopes and Run.
class WebPythonScriptContext {
 public:
  // Called synchronously after each block, before executing the next source.
  // The result is borrowed; false requires stopping the remaining sources.
  using ReportResult = base::FunctionRef<bool(const WebPythonScriptResult&)>;
  virtual ~WebPythonScriptContext() = default;
  virtual std::vector<WebPythonScriptResult> Run(
      const std::vector<WebPythonScriptSource>&,
      WebPythonDomHost&,
      ReportResult) = 0;
};

// Main-thread embedder interface for the private PythonScript experiment.
// Blink checks document eligibility and CSP before passing immutable sources.
// One context uses one interpreter for the owning Document. It has no async
// callbacks or JavaScript execution entry points.
class BLINK_EXPORT WebPythonScriptRunner {
 public:
  virtual ~WebPythonScriptRunner() = default;
  virtual bool IsOriginAllowed(const WebSecurityOrigin&) const = 0;
  virtual std::unique_ptr<WebPythonScriptContext> CreateContext() = 0;

  // Caller owns the runner and must outlive its installation. Main thread only.
  static void SetForProcess(WebPythonScriptRunner*);
  static WebPythonScriptRunner* GetForProcess();
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_PYTHON_SCRIPT_RUNNER_H_
