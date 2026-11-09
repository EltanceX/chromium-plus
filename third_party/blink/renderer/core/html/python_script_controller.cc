// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/python_script_controller.h"

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/web/web_python_script_runner.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/document_parser.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/core/dom/events/event_dispatch_forbidden_scope.h"
#include "third_party/blink/renderer/core/dom/events/native_event_listener.h"
#include "third_party/blink/renderer/core/event_type_names.h"
#include "third_party/blink/renderer/core/frame/csp/content_security_policy.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/html/html_py_script_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/platform/bindings/script_forbidden_scope.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/heap/thread_state.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/platform/scheduler/public/frame_or_worker_scheduler.h"
#include "third_party/blink/renderer/platform/scheduler/public/scheduling_policy.h"
#include "third_party/blink/renderer/platform/wtf/text/strcat.h"
#include "third_party/blink/renderer/platform/wtf/threading.h"
#include "v8/include/cppgc/heap-state.h"

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

// Shared by subscriptions created by one block (including in its callbacks).
// Strings and positions cannot keep the original pyscript node alive.
class PythonScriptSource final : public GarbageCollected<PythonScriptSource> {
 public:
  explicit PythonScriptSource(const HTMLPyScriptElement& element)
      : source(element.SourceSnapshot()),
        nonce(element.NonceSnapshot()),
        url(element.GetDocument().Url().GetString()),
        position(element.SourcePosition()) {}
  void Trace(Visitor*) const {}
  const String source;
  const String nonce;
  const String url;
  const TextPosition position;
};

class PythonClickListener final : public NativeEventListener {
 public:
  PythonClickListener(PythonScriptController& controller,
                      Element& element,
                      PythonScriptSource& source,
                      WebPythonDomHandle handle,
                      WebPythonCallbackHandle callback,
                      bool once)
      : controller(&controller),
        element(&element),
        source(&source),
        handle(handle),
        callback(callback),
        once(once) {}
  void Invoke(ExecutionContext*, Event* event) override {
    if (controller && active) {
      controller->DispatchClick(*this, *event);
    }
  }
  void Trace(Visitor* visitor) const override {
    visitor->Trace(controller);
    visitor->Trace(element);
    visitor->Trace(source);
    NativeEventListener::Trace(visitor);
  }
  WeakMember<PythonScriptController> controller;
  WeakMember<Element> element;
  Member<PythonScriptSource> source;
  const WebPythonDomHandle handle;
  const WebPythonCallbackHandle callback;
  const bool once;
  bool active = true;
};

namespace {

WebPythonScriptRunner* g_runner = nullptr;
bool g_python_running = false;
uint64_t g_next_document_id = 0;

void DestroyContextWhenSafe(
    WebPythonScriptContext* context,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  CHECK(IsMainThread());
  auto* thread_state = ThreadState::Current();
  if (g_python_running || ScriptForbiddenScope::IsScriptForbidden() ||
      ScriptForbiddenScope::WillBeScriptForbidden() ||
      thread_state->IsSweepingOnOwningThread() ||
      cppgc::subtle::HeapState::IsInAtomicPause(thread_state->heap_handle())) {
    task_runner->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&DestroyContextWhenSafe, context, task_runner),
        base::Milliseconds(1));
    return;
  }
  base::AutoReset<bool> entry(&g_python_running, true);
  delete context;
}

bool IsOrdinaryPage(Document& document) {
  const auto* origin = document.domWindow()->GetSecurityOrigin();
  // about:blank and blob documents may inherit a privileged origin.
  if (!origin->IsOpaque() && origin->Protocol() != "http" &&
      origin->Protocol() != "https" && origin->Protocol() != "file") {
    return false;
  }
  const auto& url = document.Url();
  return url.ProtocolIsInHttpFamily() || url.ProtocolIs("file") ||
         url.ProtocolIs("data") || url.ProtocolIs("blob") ||
         url.IsAboutBlankUrl() || url.IsAboutSrcdocUrl();
}

bool Eligible(Document& document, bool check_input = true) {
  auto* window = document.domWindow();
  auto* frame = document.GetFrame();
  auto* parser = document.Parser();
  return RuntimeEnabledFeatures::PythonScriptEnabled() && g_runner &&
         document.IsHTMLDocument() && !document.IsTextDocument() &&
         !document.IsInitialEmptyDocument() &&
         !document.IsDOMParserDocument() && document.IsActive() &&
         !document.IsPrerendering() && window && frame && frame->IsAttached() &&
         frame->GetDocument() == &document && !window->IsContextDestroyed() &&
         (!check_input || (parser && parser->HasPythonScriptEligibleInput())) &&
         (g_runner->AllowsUnrestrictedPages()
              ? IsOrdinaryPage(document)
              : (document.IsInOutermostMainFrame() &&
                 document.Url().ProtocolIsInHttpFamily() &&
                 !window->GetSecurityOrigin()->IsOpaque() &&
                 g_runner->IsOriginAllowed(
                     WebSecurityOrigin(window->GetSecurityOrigin()))));
}

bool ScriptsAllowed(Document& document) {
  return g_runner->AllowsUnrestrictedPages() ||
         document.domWindow()->CanExecuteScripts(kAboutToExecuteScript);
}

void Log(Document& document,
         const PythonScriptSource& source,
         const String& text,
         mojom::blink::ConsoleMessageLevel level) {
  if (text.empty() || !document.GetExecutionContext()) {
    return;
  }
  document.GetExecutionContext()->AddConsoleMessage(
      MakeGarbageCollected<ConsoleMessage>(
          mojom::blink::ConsoleMessageSource::kOther, level, text,
          MakeGarbageCollected<SourceLocation>(
              source.url, String(), source.position.line_.OneBasedInt(),
              source.position.column_.OneBasedInt(), nullptr)));
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
  visitor->Trace(listeners_);
  visitor->Trace(current_source_);
  Supplement<Document>::Trace(visitor);
  ExecutionContextLifecycleObserver::Trace(visitor);
}

PythonScriptController& PythonScriptController::GetOrCreate(
    Document& document) {
  auto* controller =
      Supplement<Document>::From<PythonScriptController>(document);
  if (!controller) {
    controller = MakeGarbageCollected<PythonScriptController>(document);
    ProvideTo(document, controller);
  }
  return *controller;
}

void PythonScriptController::AppendConsole(
    const HTMLPyScriptElement& element,
    const String& text,
    mojom::blink::ConsoleMessageLevel level) {
  AppendConsole(*MakeGarbageCollected<PythonScriptSource>(element), text,
                level);
}

void PythonScriptController::AppendConsole(
    const PythonScriptSource& source,
    const String& text,
    mojom::blink::ConsoleMessageLevel level) {
  if (text.empty() || console_truncated_) {
    return;
  }
  constexpr size_t kLimit = 16 * 1024;
  constexpr std::string_view kNotice =
      "[pyscript] Python output was truncated at 16 KiB; document execution "
      "stopped.";
  const std::string utf8 = text.Utf8();
  const size_t remaining = kLimit - kNotice.size() - console_bytes_;
  const std::string_view accepted =
      base::TruncateUTF8ToByteSize(utf8, remaining);
  Log(*GetSupplementable(), source, String::FromUtf8(accepted), level);
  console_bytes_ += accepted.size();
  if (accepted.size() != utf8.size()) {
    console_truncated_ = true;
    Log(*GetSupplementable(), source, String::FromUtf8(kNotice),
        mojom::blink::ConsoleMessageLevel::kError);
    console_bytes_ += kNotice.size();
    Dispose();
  }
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
      base::BindOnce(&DestroyContextWhenSafe, context, cleanup_task_runner_));
}

void PythonScriptController::Dispose() {
  if (!stopped_) {
    stopped_ = true;
    ++generation_;
    pending_.clear();
    handles_.clear();
    while (!listeners_.empty()) {
      DetachListener(*listeners_.back());
    }
    current_source_ = nullptr;
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
  return running_ && !stopped_ && !dom_budget_exhausted_ &&
         !ScriptForbiddenScope::IsScriptForbidden() &&
         Eligible(*document, false) && ScriptsAllowed(*document);
}

WebPythonDomResult PythonScriptController::CheckDomCall() {
  if (dom_budget_exhausted_) {
    return {{},
            WebPythonDomError::kStopped,
            "Python document exceeded the 1024 DOM call budget"};
  }
  if (!IsActive()) {
    return {
        {}, WebPythonDomError::kReference, "The document context is inactive"};
  }
  if (dom_calls_ >= 1024) {
    dom_budget_exhausted_ = true;
    ++generation_;
    handles_.clear();
    return {{},
            WebPythonDomError::kStopped,
            "Python document exceeded the 1024 DOM call budget"};
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

WebPythonDomResult PythonScriptController::AddEventListener(
    WebPythonDomHandle handle,
    const WebString& type,
    WebPythonCallbackHandle callback,
    bool once) {
  auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return status;
  }
  Element* element = ResolveElement(handle);
  if (!element) {
    return {{}, WebPythonDomError::kReference, "Invalid or expired DOM handle"};
  }
  if (type != "click" || !element->IsHTMLElement() ||
      element->GetCustomElementState() != CustomElementState::kUncustomized ||
      element->FastHasAttribute(html_names::kIsAttr)) {
    return {{},
            WebPythonDomError::kNotAllowed,
            "Python click listeners require an ordinary HTML element"};
  }
  if (!current_source_ || !callback.context || !callback.id) {
    return {{}, WebPythonDomError::kState, "No active Python callback source"};
  }
  if (listeners_.size() >= 128) {
    return {{}, WebPythonDomError::kLimit, "At most 128 Python listeners"};
  }
  for (const auto& listener : listeners_) {
    if (listener->callback.context == callback.context &&
        listener->callback.id == callback.id) {
      return {{}, WebPythonDomError::kState, "Callback is already installed"};
    }
  }
  auto* listener = MakeGarbageCollected<PythonClickListener>(
      *this, *element, *current_source_, handle, callback, once);
  listeners_.push_back(listener);
  // Each registration has a distinct native listener, preserving Blink's
  // dispatch order and add/remove-during-dispatch semantics. once is consumed
  // only after Python admission, rather than by EventTarget before admission.
  element->addEventListener(event_type_names::kClick, listener, false);
  return {};
}

WebPythonDomResult PythonScriptController::RemoveEventListener(
    WebPythonDomHandle handle,
    WebPythonCallbackHandle callback) {
  auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return status;
  }
  if (handle.document != document_id_ || handle.generation != generation_ ||
      !handle.id || handle.id > handles_.size()) {
    return {{}, WebPythonDomError::kReference, "Invalid or expired DOM handle"};
  }
  RevokeEventListener(handle, callback);
  return {};
}

void PythonScriptController::DetachListener(PythonClickListener& listener) {
  listener.active = false;
  if (listener.element) {
    listener.element->removeEventListener(event_type_names::kClick, &listener,
                                          false);
  }
  auto index = listeners_.Find(&listener);
  if (index != kNotFound) {
    listeners_.EraseAt(index);
  }
}

void PythonScriptController::RevokeEventListener(
    WebPythonDomHandle handle,
    WebPythonCallbackHandle callback) {
  for (const auto& listener : listeners_) {
    if (listener->handle.document == handle.document &&
        listener->handle.generation == handle.generation &&
        listener->handle.id == handle.id &&
        listener->callback.context == callback.context &&
        listener->callback.id == callback.id) {
      DetachListener(*listener);
      return;
    }
  }
}

Element* PythonScriptController::ResolveElement(
    WebPythonDomHandle handle) const {
  if (handle.document != document_id_ || handle.generation != generation_ ||
      !handle.id || handle.id > handles_.size()) {
    return nullptr;
  }
  Element* element = handles_[static_cast<wtf_size_t>(handle.id - 1)]->element;
  return element && &element->GetDocument() == GetSupplementable() &&
                 element->isConnected() && !element->IsInShadowTree()
             ? element
             : nullptr;
}

void PythonScriptController::DispatchClick(PythonClickListener& listener,
                                           Event& event) {
  if (stopped_ || !listener.active || !context_ ||
      event.type() != event_type_names::kClick) {
    return;
  }
  Element* element = ResolveElement(listener.handle);
  if (!element || element != listener.element ||
      event.currentTarget() != element || !element->IsHTMLElement() ||
      element->GetCustomElementState() != CustomElementState::kUncustomized ||
      element->FastHasAttribute(html_names::kIsAttr)) {
    return;
  }
  Document& document = *GetSupplementable();
  auto* thread = ThreadState::Current();
  if (thread->IsSweepingOnOwningThread() ||
      cppgc::subtle::HeapState::IsInAtomicPause(thread->heap_handle())) {
    return;
  }
  const bool safe =
      !g_python_running && !running_ &&
      !ScriptForbiddenScope::IsScriptForbidden() &&
      !ScriptForbiddenScope::WillBeScriptForbidden() &&
      !thread->IsSweepingOnOwningThread() &&
      !cppgc::subtle::HeapState::IsInAtomicPause(thread->heap_handle());
  if (!safe || !Eligible(document, false) || !ScriptsAllowed(document)) {
    // Never queue a skipped click. Keep diagnostics bounded for this document.
    if (!dispatch_warning_reported_) {
      dispatch_warning_reported_ = true;
      AppendConsole(*listener.source,
                    "[pyscript] Python click skipped: document "
                    "policy or execution state does not allow callbacks.",
                    mojom::blink::ConsoleMessageLevel::kWarning);
    }
    return;
  }
  Persistent<PythonScriptController> protect(this);
  Persistent<PythonClickListener> protect_listener(&listener);
  Persistent<PythonScriptSource> source(listener.source);
  if (!g_runner->AllowsUnrestrictedPages() &&
      !document.domWindow()->GetContentSecurityPolicy()->AllowInline(
          ContentSecurityPolicy::InlineType::kScript, nullptr, source->source,
          source->nonce, source->url, source->position)) {
    return;
  }
  base::AutoReset<bool> entry(&g_python_running, true);
  running_ = true;
  current_source_ = source;
  if (listener.once) {
    DetachListener(listener);
  }
  const auto result = context_->InvokeCallback(
      listener.callback, {"click", listener.handle, event.isTrusted()}, *this);
  const String prefix = StrCat(
      {"[pyscript callback ", String::Number(listener.callback.id), "] "});
  if (!result.stdout_text.IsEmpty()) {
    AppendConsole(*source, StrCat({prefix, result.stdout_text}),
                  mojom::blink::ConsoleMessageLevel::kInfo);
  }
  if (!result.stderr_text.IsEmpty()) {
    AppendConsole(*source, StrCat({prefix, result.stderr_text}),
                  mojom::blink::ConsoleMessageLevel::kError);
  }
  if (!result.success) {
    AppendConsole(*source, StrCat({prefix, result.error}),
                  mojom::blink::ConsoleMessageLevel::kError);
  }
  if (result.stopped || dom_budget_exhausted_ || !Eligible(document, false)) {
    Dispose();
  }
  current_source_ = nullptr;
  running_ = false;
  if (stopped_) {
    ReleaseContext();
  }
}

void PythonScriptController::Report(Document& document,
                                    const HTMLPyScriptElement& element,
                                    const String& text) {
  GetOrCreate(document).AppendConsole(
      element, StrCat({"[pyscript] ", text}),
      mojom::blink::ConsoleMessageLevel::kError);
}

void PythonScriptController::Register(HTMLPyScriptElement& element) {
  Document& document = element.GetDocument();
  auto* controller = &GetOrCreate(document);
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
  if (!ScriptsAllowed(document)) {
    return;
  }

  std::vector<WebPythonScriptSource> sources;
  HeapVector<Member<HTMLPyScriptElement>> accepted;
  HeapVector<Member<PythonScriptSource>> authorizations;
  unsigned index = 0;
  for (const auto& block : pending) {
    ++index;
    auto* element = block->element.Get();
    if (!element || &element->GetDocument() != &document ||
        !element->isConnected() || element->IsInShadowTree() ||
        element->HasUnsupportedAttributes()) {
      continue;
    }
    if (!g_runner->AllowsUnrestrictedPages() &&
        !document.domWindow()->GetContentSecurityPolicy()->AllowInline(
            ContentSecurityPolicy::InlineType::kScript, element,
            element->SourceSnapshot(), element->NonceSnapshot(), document.Url(),
            element->SourcePosition())) {
      continue;
    }
    sources.push_back({element->SourceSnapshot(),
                       StrCat({document.Url().GetString(), "#pyscript-",
                               String::Number(index)})});
    accepted.push_back(element);
    authorizations.push_back(
        MakeGarbageCollected<PythonScriptSource>(*element));
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
  controller->current_source_ = authorizations.front();
  wtf_size_t reported = 0;
  auto report_result = [&](const WebPythonScriptResult& result) {
    if (controller->stopped_ || !Eligible(document)) {
      controller->Dispose();
      return false;
    }
    CHECK_LT(reported, accepted.size());
    const auto& element = *accepted[reported++];
    controller->AppendConsole(element, result.stdout_text,
                              mojom::blink::ConsoleMessageLevel::kInfo);
    controller->AppendConsole(element, result.stderr_text,
                              mojom::blink::ConsoleMessageLevel::kError);
    if (!result.success) {
      Report(document, element, result.error);
    }
    if (result.output_truncated && !result.stopped) {
      Report(document, element, "Python output was truncated at 16 KiB.");
    }
    if (result.stopped || controller->console_truncated_) {
      controller->Dispose();
      return false;
    }
    controller->current_source_ = reported < authorizations.size()
                                      ? authorizations[reported].Get()
                                      : nullptr;
    return true;
  };
  const auto results =
      controller->context_->Run(sources, *controller, report_result);
  controller->running_ = false;
  controller->current_source_ = nullptr;
  if (controller->stopped_) {
    controller->ReleaseContext();
    return;
  }
  CHECK_EQ(results.size(), reported);
  CHECK_EQ(results.size(), accepted.size());
  if (controller->dom_budget_exhausted_) {
    // Revoke even if a test embedder omitted the fatal result flag.
    Report(document, *accepted.back(),
           "Python document exceeded the 1024 DOM call budget");
    controller->Dispose();
  }
}

}  // namespace blink
