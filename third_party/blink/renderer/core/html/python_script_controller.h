// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PYTHON_SCRIPT_CONTROLLER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PYTHON_SCRIPT_CONTROLLER_H_

#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "third_party/blink/public/mojom/devtools/console_message.mojom-blink-forward.h"
#include "third_party/blink/public/web/web_python_script_runner.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/execution_context/execution_context_lifecycle_observer.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_vector.h"
#include "third_party/blink/renderer/platform/heap/prefinalizer.h"
#include "third_party/blink/renderer/platform/supplementable.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class Document;
class Element;
class Event;
class HTMLPyScriptElement;
class PendingPythonScriptBlock;
class PythonDomHandleEntry;
class PythonClickListener;
class PythonScriptSource;

class PythonScriptController final
    : public GarbageCollected<PythonScriptController>,
      public Supplement<Document>,
      public ExecutionContextLifecycleObserver,
      public WebPythonDomHost {
  USING_PRE_FINALIZER(PythonScriptController, Dispose);

 public:
  static const char kSupplementName[];
  explicit PythonScriptController(Document&);
  static void Register(HTMLPyScriptElement&);
  static void ExecutePending(Document&);
  CORE_EXPORT static void StopDocument(Document&);
  static void Report(Document&, const HTMLPyScriptElement&, const String&);
  void Trace(Visitor*) const override;
  void ContextDestroyed() override;
  bool IsActive() const override;
  WebPythonDomResult GetElementById(const WebString&) override;
  WebPythonDomResult SetText(WebPythonDomHandle, const WebString&) override;
  WebPythonDomResult AddEventListener(WebPythonDomHandle,
                                      const WebString&,
                                      WebPythonCallbackHandle,
                                      bool once) override;
  WebPythonDomResult RemoveEventListener(WebPythonDomHandle,
                                         WebPythonCallbackHandle) override;
  void RevokeEventListener(WebPythonDomHandle,
                           WebPythonCallbackHandle) override;

 private:
  friend class PythonClickListener;
  static PythonScriptController& GetOrCreate(Document&);
  void AppendConsole(const HTMLPyScriptElement&,
                     const String&,
                     mojom::blink::ConsoleMessageLevel);
  void AppendConsole(const PythonScriptSource&,
                     const String&,
                     mojom::blink::ConsoleMessageLevel);
  void DispatchClick(PythonClickListener&, Event&);
  void DetachListener(PythonClickListener&);
  Element* ResolveElement(WebPythonDomHandle) const;
  void Dispose();
  void ReleaseContext();
  WebPythonDomResult CheckDomCall();
  HeapVector<Member<PendingPythonScriptBlock>> pending_;
  HeapVector<Member<PythonDomHandleEntry>> handles_;
  HeapVector<Member<PythonClickListener>> listeners_;
  Member<PythonScriptSource> current_source_;
  std::unique_ptr<WebPythonScriptContext> context_;
  scoped_refptr<base::SingleThreadTaskRunner> cleanup_task_runner_;
  const uint64_t document_id_;
  uint64_t generation_ = 1;
  unsigned dom_calls_ = 0;
  unsigned callback_dom_calls_ = 0;
  size_t console_bytes_ = 0;
  bool console_truncated_ = false;
  bool dom_budget_exhausted_ = false;
  bool running_ = false;
  bool stopped_ = false;
  bool consumed_ = false;
  bool dispatch_warning_reported_ = false;
  bool callback_running_ = false;
  base::TimeTicks last_window_warning_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PYTHON_SCRIPT_CONTROLLER_H_
