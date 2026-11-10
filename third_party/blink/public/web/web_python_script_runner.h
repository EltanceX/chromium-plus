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
  bool callback_skipped = false;
};

struct WebPythonDomHandle {
  uint64_t document = 0;
  uint64_t generation = 0;
  uint64_t id = 0;
};

struct WebPythonCallbackHandle {
  uint64_t context = 0;
  uint64_t id = 0;
};

struct WebPythonCallbackEvent {
  WebString type;
  WebPythonDomHandle current_target;
  bool is_trusted = false;
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

// Borrowed during synchronous Run/InvokeCallback only. Values crossing this
// boundary never contain Nodes or Python objects.
class WebPythonDomHost {
 public:
  virtual ~WebPythonDomHost() = default;
  virtual bool IsActive() const = 0;
  virtual WebPythonDomResult GetElementById(const WebString&) = 0;
  virtual WebPythonDomResult SetText(WebPythonDomHandle, const WebString&) = 0;
  virtual WebPythonDomResult AddEventListener(WebPythonDomHandle,
                                              const WebString& type,
                                              WebPythonCallbackHandle,
                                              bool once) = 0;
  virtual WebPythonDomResult RemoveEventListener(WebPythonDomHandle,
                                                 WebPythonCallbackHandle) = 0;
  // Internal idempotent rollback, even after activity/budget admission fails.
  // Must not execute script or Python. Not a page API or budgeted DOM call.
  virtual void RevokeEventListener(WebPythonDomHandle,
                                   WebPythonCallbackHandle) = 0;
  virtual WebPythonDomResult RequestFrame(WebPythonCallbackHandle) = 0;
  virtual WebPythonDomResult CancelFrame(WebPythonCallbackHandle) = 0;
  virtual void RevokeFrame(WebPythonCallbackHandle) = 0;
};

// Owned by a Document controller. Destruction can execute Python finalizers and
// must happen on the main thread outside GC, script-forbidden scopes and any
// active Python execution.
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
  // The caller checks document policy and reports this synchronous result.
  virtual WebPythonScriptResult InvokeCallback(WebPythonCallbackHandle,
                                               const WebPythonCallbackEvent&,
                                               WebPythonDomHost&) = 0;
  // Probe immediately before consuming a native once listener. The caller
  // permits no intervening Python entry; idle time only frees capacity.
  virtual bool CanInvokeCallback() { return true; }
  virtual bool CanInvokeFrameCallback() { return false; }
  virtual WebPythonScriptResult InvokeFrameCallback(WebPythonCallbackHandle,
                                                    double timestamp_ms,
                                                    WebPythonDomHost&) = 0;
  // Registry mutation only, including consumption from an active host call.
  virtual WebPythonScriptResult RemoveCallback(WebPythonCallbackHandle) = 0;
};

// Main-thread embedder interface for the private PythonScript experiment.
// Blink checks document eligibility and, in restricted mode, script policy/CSP
// before passing immutable sources.
// One context uses one interpreter for the owning Document. Browser-scheduled
// frame callbacks enter synchronously; no JavaScript execution entry is
// exposed.
class BLINK_EXPORT WebPythonScriptRunner {
 public:
  virtual ~WebPythonScriptRunner() = default;
  virtual bool IsOriginAllowed(const WebSecurityOrigin&) const = 0;
  // Explicit browser-user override of ordinary page/origin and script policy.
  // Does not change OS privileges, document lifetime or parser-only semantics.
  virtual bool AllowsUnrestrictedPages() const { return false; }
  virtual std::unique_ptr<WebPythonScriptContext> CreateContext() = 0;

  // Caller owns the runner and must outlive its installation. Main thread only.
  static void SetForProcess(WebPythonScriptRunner*);
  static WebPythonScriptRunner* GetForProcess();
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_PYTHON_SCRIPT_RUNNER_H_
