// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/python_script_controller.h"

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/web/web_python_script_runner.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/document_parser.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/dom/events/event_dispatch_forbidden_scope.h"
#include "third_party/blink/renderer/core/frame/csp/content_security_policy.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/html/html_py_script_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/platform/bindings/script_forbidden_scope.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/platform/scheduler/public/frame_or_worker_scheduler.h"
#include "third_party/blink/renderer/platform/scheduler/public/scheduling_policy.h"
#include "third_party/blink/renderer/platform/wtf/text/strcat.h"
#include "third_party/blink/renderer/platform/wtf/threading.h"

namespace blink {

// HeapVector does not support weak entries directly. Keep order and let a
// collected element disappear without retaining it until parsing completes.
class PendingPythonScriptBlock final
    : public GarbageCollected<PendingPythonScriptBlock> {
 public:
  explicit PendingPythonScriptBlock(HTMLPyScriptElement& element)
      : element(&element) {}
  void Trace(Visitor* visitor) const { visitor->Trace(element); }
  WeakMember<HTMLPyScriptElement> element;
};

class PythonDomHandleEntry final
    : public GarbageCollected<PythonDomHandleEntry> {
 public:
  explicit PythonDomHandleEntry(Element& value) : element(&value) {}
  void Trace(Visitor* visitor) const { visitor->Trace(element); }
  WeakMember<Element> element;
};

namespace {

WebPythonScriptRunner* g_runner = nullptr;
bool g_python_running = false;
uint64_t g_next_document_id = 0;

bool Eligible(Document& document, bool check_input = true) {
  auto* window = document.domWindow();
  auto* frame = document.GetFrame();
  auto* parser = document.Parser();
  return RuntimeEnabledFeatures::PythonScriptEnabled() && g_runner &&
         document.IsHTMLDocument() && !document.IsTextDocument() &&
         !document.IsInitialEmptyDocument() &&
         !document.IsDOMParserDocument() && document.IsActive() &&
         !document.IsPrerendering() && document.IsInOutermostMainFrame() &&
         document.Url().ProtocolIsInHttpFamily() && window && frame &&
         frame->IsAttached() && frame->GetDocument() == &document &&
         !window->IsContextDestroyed() &&
         (!check_input || (parser && parser->HasPythonScriptEligibleInput())) &&
         !window->GetSecurityOrigin()->IsOpaque() &&
         g_runner->IsOriginAllowed(
             WebSecurityOrigin(window->GetSecurityOrigin()));
}

void Log(Document& document,
         const HTMLPyScriptElement& element,
         const String& text,
         mojom::blink::ConsoleMessageLevel level) {
  if (text.empty() || !document.GetExecutionContext()) {
    return;
  }
  document.GetExecutionContext()->AddConsoleMessage(
      MakeGarbageCollected<ConsoleMessage>(
          mojom::blink::ConsoleMessageSource::kOther, level, text,
          MakeGarbageCollected<SourceLocation>(
              document.Url().GetString(), String(),
              element.SourcePosition().line_.OneBasedInt(),
              element.SourcePosition().column_.OneBasedInt(), nullptr)));
}

}  // namespace

void WebPythonScriptRunner::SetForProcess(WebPythonScriptRunner* runner) {
  CHECK(IsMainThread());
  g_runner = runner;
}

WebPythonScriptRunner* WebPythonScriptRunner::GetForProcess() {
  CHECK(IsMainThread());
  return g_runner;
}

const char PythonScriptController::kSupplementName[] = "PythonScriptController";

PythonScriptController::PythonScriptController(Document& document)
    : Supplement<Document>(document),
      ExecutionContextLifecycleObserver(document.GetExecutionContext()),
      cleanup_task_runner_(document.GetTaskRunner(TaskType::kInternalDefault)),
      document_id_(++g_next_document_id) {
  CHECK(document_id_);
}

void PythonScriptController::Trace(Visitor* visitor) const {
  visitor->Trace(pending_);
  visitor->Trace(handles_);
  Supplement<Document>::Trace(visitor);
  ExecutionContextLifecycleObserver::Trace(visitor);
}

void PythonScriptController::ReleaseContext() {
  if (!context_ || running_) {
    return;
  }
  // Do not destroy an interpreter in an Oilpan finalizer or a script-forbidden
  // lifecycle callback. Only the task body owns deletion. If the task queue is
  // shut down, intentionally leave this process-local resource to process exit
  // rather than executing Python while a discarded task closure is destroyed.
  auto* context = context_.release();
  cleanup_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce([](WebPythonScriptContext* value) { delete value; },
                     context));
}

void PythonScriptController::Dispose() {
  if (!stopped_) {
    stopped_ = true;
    ++generation_;
    pending_.clear();
    handles_.clear();
  }
  ReleaseContext();
}

void PythonScriptController::StopDocument(Document& document) {
  if (auto* controller =
          Supplement<Document>::From<PythonScriptController>(document)) {
    controller->Dispose();
  }
}

void PythonScriptController::ContextDestroyed() {
  Dispose();
}

bool PythonScriptController::IsActive() const {
  Document* document = GetSupplementable();
  return running_ && !stopped_ && !ScriptForbiddenScope::IsScriptForbidden() &&
         Eligible(*document, false) &&
         document->domWindow()->CanExecuteScripts(kAboutToExecuteScript);
}

WebPythonDomResult PythonScriptController::CheckDomCall() {
  if (!IsActive()) {
    return {
        {}, WebPythonDomError::kReference, "The document context is inactive"};
  }
  if (dom_calls_ >= 1024) {
    return {
        {}, WebPythonDomError::kLimit, "At most 1024 DOM calls per document"};
  }
  ++dom_calls_;
  return {};
}

WebPythonDomResult PythonScriptController::GetElementById(const WebString& id) {
  auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return status;
  }
  if (id.length() > 16384 || id.Utf8().size() > 16384) {
    return {
        {}, WebPythonDomError::kLimit, "DOM strings must not exceed 16 KiB"};
  }
  Element* element = GetSupplementable()->getElementById(AtomicString(id));
  if (!element || element->IsInShadowTree()) {
    return {};
  }
  for (wtf_size_t i = 0; i < handles_.size(); ++i) {
    if (handles_[i]->element == element) {
      return {{document_id_, generation_, i + 1}};
    }
  }
  handles_.push_back(MakeGarbageCollected<PythonDomHandleEntry>(*element));
  return {{document_id_, generation_, handles_.size()}};
}

WebPythonDomResult PythonScriptController::SetText(WebPythonDomHandle handle,
                                                   const WebString& value) {
  auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return status;
  }
  if (handle.document != document_id_ || handle.generation != generation_ ||
      !handle.id || handle.id > handles_.size()) {
    return {{}, WebPythonDomError::kReference, "Invalid or expired DOM handle"};
  }
  Element* element =
      handles_[static_cast<wtf_size_t>(handle.id - 1)]->element.Get();
  if (!element || &element->GetDocument() != GetSupplementable() ||
      !element->isConnected() || element->IsInShadowTree()) {
    return {{},
            WebPythonDomError::kReference,
            "The DOM node is no longer available"};
  }
  if ((!element->HasTagName(html_names::kPTag) &&
       !element->HasTagName(html_names::kDivTag) &&
       !element->HasTagName(html_names::kSpanTag)) ||
      element->GetCustomElementState() != CustomElementState::kUncustomized ||
      element->FastHasAttribute(html_names::kIsAttr) ||
      element->firstElementChild()) {
    return {{},
            WebPythonDomError::kNotAllowed,
            "set_text requires an ordinary p, div or span without element "
            "children"};
  }
  if (value.length() > 16384 || value.Utf8().size() > 16384) {
    return {
        {}, WebPythonDomError::kLimit, "DOM text must not exceed 16 KiB UTF-8"};
  }
  {
    // Replacing text/comment children of these native display elements has no
    // custom-element reactions. Guard against any unexpected synchronous script
    // or event entry; mutation observers run after the Python stack returns.
    ScriptForbiddenScope forbid_script;
    EventDispatchForbiddenScope forbid_events;
    element->setTextContent(value);
  }
  return IsActive()
             ? WebPythonDomResult{}
             : WebPythonDomResult{{},
                                  WebPythonDomError::kReference,
                                  "The document changed during the DOM call"};
}

void PythonScriptController::Report(Document& document,
                                    const HTMLPyScriptElement& element,
                                    const String& text) {
  Log(document, element, StrCat({"[pyscript] ", text}),
      mojom::blink::ConsoleMessageLevel::kError);
}

void PythonScriptController::Register(HTMLPyScriptElement& element) {
  Document& document = element.GetDocument();
  auto* controller =
      Supplement<Document>::From<PythonScriptController>(document);
  if (!controller) {
    controller = MakeGarbageCollected<PythonScriptController>(document);
    ProvideTo(document, controller);
  }
  if (controller->consumed_ || controller->stopped_) {
    return;
  }
  if (controller->pending_.size() == 16) {
    Report(document, element,
           "At most 16 Python blocks are accepted per document.");
    return;
  }
  controller->pending_.push_back(
      MakeGarbageCollected<PendingPythonScriptBlock>(element));
}

void PythonScriptController::ExecutePending(Document& document) {
  auto* controller =
      Supplement<Document>::From<PythonScriptController>(document);
  if (!controller || controller->consumed_ || controller->stopped_) {
    return;
  }
  controller->consumed_ = true;
  HeapVector<Member<PendingPythonScriptBlock>> pending;
  pending.swap(controller->pending_);
  if (g_python_running || ScriptForbiddenScope::IsScriptForbidden() ||
      !Eligible(document)) {
    return;
  }
  if (!document.domWindow()->CanExecuteScripts(kAboutToExecuteScript)) {
    return;
  }

  std::vector<WebPythonScriptSource> sources;
  HeapVector<Member<HTMLPyScriptElement>> accepted;
  unsigned index = 0;
  for (const auto& block : pending) {
    ++index;
    auto* element = block->element.Get();
    if (!element || &element->GetDocument() != &document ||
        !element->isConnected() || element->IsInShadowTree() ||
        element->HasUnsupportedAttributes()) {
      continue;
    }
    if (!document.domWindow()->GetContentSecurityPolicy()->AllowInline(
            ContentSecurityPolicy::InlineType::kScript, element,
            element->SourceSnapshot(), element->NonceSnapshot(), document.Url(),
            element->SourcePosition())) {
      continue;
    }
    sources.push_back({element->SourceSnapshot(),
                       StrCat({document.Url().GetString(), "#pyscript-",
                               String::Number(index)})});
    accepted.push_back(element);
  }
  if (sources.empty() || !Eligible(document)) {
    return;
  }
  Persistent<PythonScriptController> protect(controller);
  base::AutoReset<bool> entry(&g_python_running, true);
  controller->context_ = g_runner->CreateContext();
  if (!controller->context_) {
    Report(document, *accepted.front(), "Could not create the Python context");
    return;
  }
  document.GetExecutionContext()->GetScheduler()->RegisterStickyFeature(
      SchedulingPolicy::Feature::kNativePython,
      {SchedulingPolicy::DisableBackForwardCache()});
  controller->running_ = true;
  const auto results = controller->context_->Run(sources, *controller);
  controller->running_ = false;
  if (controller->stopped_) {
    controller->ReleaseContext();
    return;
  }
  if (!Eligible(document)) {
    controller->Dispose();
    return;
  }
  CHECK_EQ(results.size(), accepted.size());
  for (wtf_size_t i = 0; i < accepted.size(); ++i) {
    const auto& result = results[i];
    const auto& element = *accepted[i];
    Log(document, element, result.stdout_text,
        mojom::blink::ConsoleMessageLevel::kInfo);
    Log(document, element, result.stderr_text,
        mojom::blink::ConsoleMessageLevel::kError);
    if (!result.success) {
      Report(document, element, result.error);
    }
    if (result.output_truncated) {
      Report(document, element, "Python output was truncated at 16 KiB.");
    }
  }
}

}  // namespace blink
