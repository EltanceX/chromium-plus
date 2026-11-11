// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/python_script_controller.h"

#include <cmath>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "third_party/abseil-cpp/absl/cleanup/cleanup.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/web/web_python_script_runner.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_focus_options.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_scroll_into_view_options.h"
#include "third_party/blink/renderer/core/css/css_primitive_value.h"
#include "third_party/blink/renderer/core/css/media_values.h"
#include "third_party/blink/renderer/core/css/properties/css_property.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/document_parser.h"
#include "third_party/blink/renderer/core/dom/dom_token_list.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/dom/events/add_event_listener_options_resolved.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/core/dom/events/event_dispatch_forbidden_scope.h"
#include "third_party/blink/renderer/core/dom/events/native_event_listener.h"
#include "third_party/blink/renderer/core/dom/frame_request_callback_collection.h"
#include "third_party/blink/renderer/core/editing/editing_utilities.h"
#include "third_party/blink/renderer/core/event_type_names.h"
#include "third_party/blink/renderer/core/events/keyboard_event.h"
#include "third_party/blink/renderer/core/events/pointer_event.h"
#include "third_party/blink/renderer/core/frame/csp/content_security_policy.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/html/canvas/html_canvas_element.h"
#include "third_party/blink/renderer/core/html/forms/html_form_control_element.h"
#include "third_party/blink/renderer/core/html/forms/html_input_element.h"
#include "third_party/blink/renderer/core/html/forms/html_select_element.h"
#include "third_party/blink/renderer/core/html/forms/html_text_area_element.h"
#include "third_party/blink/renderer/core/html/html_dialog_element.h"
#include "third_party/blink/renderer/core/html/html_py_script_element.h"
#include "third_party/blink/renderer/core/html/python_browser_bridge.h"
#include "third_party/blink/renderer/core/html/python_canvas_bridge.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/core/layout/adjust_for_absolute_zoom.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/bindings/script_forbidden_scope.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/heap/thread_state.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/platform/scheduler/public/frame_or_worker_scheduler.h"
#include "third_party/blink/renderer/platform/scheduler/public/scheduling_policy.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
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

class PythonEventListener final : public NativeEventListener {
 public:
  PythonEventListener(PythonScriptController& controller,
                      EventTarget& target,
                      PythonScriptSource& source,
                      WebPythonDomHandle handle,
                      WebPythonCallbackHandle callback,
                      const AtomicString& type,
                      bool once,
                      bool passive,
                      bool capture)
      : controller(&controller),
        target(&target),
        source(&source),
        handle(handle),
        callback(callback),
        type(type),
        once(once),
        passive(passive),
        capture(capture) {}
  void Invoke(ExecutionContext*, Event* event) override {
    if (controller && active) {
      controller->DispatchEvent(*this, *event);
    }
  }
  void Trace(Visitor* visitor) const override {
    visitor->Trace(controller);
    visitor->Trace(target);
    visitor->Trace(source);
    NativeEventListener::Trace(visitor);
  }
  WeakMember<PythonScriptController> controller;
  WeakMember<EventTarget> target;
  Member<PythonScriptSource> source;
  const WebPythonDomHandle handle;
  const WebPythonCallbackHandle callback;
  const AtomicString type;
  const bool once;
  const bool passive;
  const bool capture;
  bool active = true;
};

// These native observers never enter Python or depend on callback admission.
class PythonInputResetListener final : public NativeEventListener {
 public:
  explicit PythonInputResetListener(PythonScriptController& value)
      : controller(&value) {}
  void Invoke(ExecutionContext*, Event* event) override {
    if (event->target() != event->currentTarget()) {
      return;
    }
    if (controller) {
      controller->ResetInput();
    }
  }
  void Trace(Visitor* visitor) const override {
    visitor->Trace(controller);
    NativeEventListener::Trace(visitor);
  }
  WeakMember<PythonScriptController> controller;
};

class PythonPointerCapture final
    : public GarbageCollected<PythonPointerCapture> {
 public:
  PythonPointerCapture(Element& value, int32_t id) : element(&value), id(id) {}
  void Trace(Visitor* visitor) const { visitor->Trace(element); }
  WeakMember<Element> element;
  const int32_t id;
};

// No Python objects cross this boundary. The source snapshot outlives removal
// of the original tag; the weak controller cannot keep a discarded page alive.
class PythonAnimationFrame final : public FrameCallback {
 public:
  PythonAnimationFrame(PythonScriptController& controller,
                       PythonScriptSource& source,
                       WebPythonCallbackHandle callback)
      : controller(&controller), source(&source), callback(callback) {}
  void Invoke(double timestamp_ms) override {
    SetId(0);
    if (controller) {
      controller->DispatchFrame(*this, timestamp_ms);
    }
  }
  void Trace(Visitor* visitor) const override {
    visitor->Trace(controller);
    visitor->Trace(source);
    FrameCallback::Trace(visitor);
  }
  const char* GetHumanReadableName() const override {
    return "PythonAnimationFrame";
  }
  WeakMember<PythonScriptController> controller;
  Member<PythonScriptSource> source;
  const WebPythonCallbackHandle callback;
};

namespace {

WebPythonScriptRunner* g_runner = nullptr;
bool g_python_running = false;
uint64_t g_next_document_id = 0;

bool IsPythonUiElement(Element* element) {
  if (!element ||
      element->GetCustomElementState() != CustomElementState::kUncustomized ||
      element->FastHasAttribute(html_names::kIsAttr)) {
    return false;
  }
  for (const auto* tag : {&html_names::kDivTag,      &html_names::kSpanTag,
                          &html_names::kPTag,        &html_names::kButtonTag,
                          &html_names::kInputTag,    &html_names::kSelectTag,
                          &html_names::kTextareaTag, &html_names::kDialogTag,
                          &html_names::kCanvasTag,   &html_names::kCodeTag,
                          &html_names::kPreTag,      &html_names::kOutputTag,
                          &html_names::kH1Tag,       &html_names::kH2Tag,
                          &html_names::kH3Tag,       &html_names::kH4Tag,
                          &html_names::kLiTag,       &html_names::kUlTag,
                          &html_names::kSectionTag,  &html_names::kAsideTag,
                          &html_names::kMainTag,     &html_names::kHeaderTag,
                          &html_names::kFooterTag,   &html_names::kNavTag,
                          &html_names::kStrongTag,   &html_names::kLabelTag}) {
    if (element->HasTagName(*tag)) {
      return true;
    }
  }
  return false;
}

bool IsPythonValueInput(HTMLInputElement* input) {
  return input && (input->type() == "range" || input->type() == "text" ||
                   input->type() == "number");
}

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
  visitor->Trace(canvas_bridge_);
  visitor->Trace(browser_bridge_);
  visitor->Trace(handles_);
  visitor->Trace(listeners_);
  visitor->Trace(pending_frame_);
  visitor->Trace(input_reset_listener_);
  visitor->Trace(pointer_captures_);
  visitor->Trace(active_event_);
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
  if (text.empty() || console_truncated_ || stopped_) {
    return;
  }
  if (callback_running_) {
    // Runtime meters callback stdout/stderr/traceback UTF-8 once, including
    // the sliding window. Fixed browser labels/notices are not Python output.
    // Do not charge events to the initialization console's lifetime allowance.
    Log(*GetSupplementable(), source, text, level);
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
    if (browser_bridge_) {
      browser_bridge_->Dispose();
      browser_bridge_ = nullptr;
    }
    if (canvas_bridge_) {
      canvas_bridge_->Dispose();
      canvas_bridge_ = nullptr;
    }
    if (pending_frame_) {
      RevokeFrame(pending_frame_->callback);
    }
    active_event_ = nullptr;
    active_event_token_ = 0;
    ReleasePointerCaptures();
    if (input_reset_listener_) {
      GetSupplementable()->removeEventListener(
          event_type_names::kVisibilitychange, input_reset_listener_, true);
      if (auto* window = GetSupplementable()->domWindow()) {
        window->removeEventListener(event_type_names::kBlur,
                                    input_reset_listener_, true);
      }
      input_reset_listener_ = nullptr;
    }
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
  auto& calls = callback_running_ ? callback_dom_calls_ : dom_calls_;
  const unsigned limit = callback_running_ ? 128 : 1024;
  if (calls >= limit) {
    dom_budget_exhausted_ = true;
    ++generation_;
    handles_.clear();
    return {{},
            WebPythonDomError::kStopped,
            WebString::FromUtf8(
                callback_running_
                    ? "Python callback exceeded the 128 DOM call budget"
                    : "Python document exceeded the 1024 DOM call budget")};
  }
  ++calls;
  return {};
}

WebPythonBrowserResult PythonScriptController::Browser(
    const WebPythonBrowserRequest& request) {
  const auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return {status};
  }
  Element* element = nullptr;
  if (request.op == WebPythonBrowserOp::kRequestFullscreen) {
    element = ResolveElement(request.element);
    if (!IsPythonUiElement(element)) {
      return {{{}, WebPythonDomError::kReference, "Expired fullscreen target"}};
    }
  }
  if (!browser_bridge_) {
    browser_bridge_ = PythonBrowserBridge::Create(
        *GetSupplementable(), BindRepeating(
                                  [](PythonScriptController* controller) {
                                    return controller &&
                                           controller->CanUseBrowserAsync();
                                  },
                                  WrapWeakPersistent(this)));
  }
  return browser_bridge_
             ? browser_bridge_->Run(request, element)
             : WebPythonBrowserResult{
                   {{}, WebPythonDomError::kState, "Browser is unavailable"}};
}

bool PythonScriptController::CanUseBrowserAsync() const {
  return !stopped_ && !dom_budget_exhausted_ &&
         !ScriptForbiddenScope::IsScriptForbidden() &&
         Eligible(*GetSupplementable(), false) &&
         ScriptsAllowed(*GetSupplementable());
}

PythonCanvasBridge* PythonScriptController::CanvasBridge() {
  if (!canvas_bridge_) {
    canvas_bridge_ = PythonCanvasBridge::Create(*GetSupplementable(),
                                                document_id_, generation_);
  }
  return canvas_bridge_;
}
WebPythonCanvasResult PythonScriptController::GetCanvasContext(
    WebPythonDomHandle handle) {
  auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return {status};
  }
  auto* element = ResolveTarget(handle);
  auto* canvas =
      element ? DynamicTo<HTMLCanvasElement>(element->ToNode()) : nullptr;
  if (!canvas) {
    return {{{},
             WebPythonDomError::kNotAllowed,
             "Expected an ordinary connected HTML canvas"}};
  }
  auto* bridge = CanvasBridge();
  return bridge ? bridge->GetContext(*canvas)
                : WebPythonCanvasResult{{{},
                                         WebPythonDomError::kNotAllowed,
                                         "Canvas is unavailable"}};
}
WebPythonCanvasResult PythonScriptController::CreateCanvasSurface(uint32_t w,
                                                                  uint32_t h) {
  auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return {status};
  }
  auto* bridge = CanvasBridge();
  return bridge ? bridge->CreateSurface(w, h)
                : WebPythonCanvasResult{{{},
                                         WebPythonDomError::kNotAllowed,
                                         "Canvas is unavailable"}};
}
WebPythonCanvasResult PythonScriptController::ResizeCanvas(
    WebPythonCanvasHandle handle,
    uint32_t w,
    uint32_t h) {
  auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return {status};
  }
  return canvas_bridge_ ? canvas_bridge_->Resize(handle, w, h)
                        : WebPythonCanvasResult{{{},
                                                 WebPythonDomError::kReference,
                                                 "Expired Canvas handle"}};
}
WebPythonDomResult PythonScriptController::ReleaseCanvas(
    WebPythonCanvasHandle handle) {
  auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return status;
  }
  return canvas_bridge_
             ? canvas_bridge_->Release(handle)
             : WebPythonDomResult{
                   {}, WebPythonDomError::kReference, "Expired Canvas handle"};
}
WebPythonCanvasResult PythonScriptController::SubmitCanvas(
    WebPythonCanvasHandle handle,
    const std::vector<WebPythonCanvasCommand>& commands) {
  auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return {status};
  }
  if (!canvas_bridge_) {
    return {{{}, WebPythonDomError::kReference, "Expired Canvas handle"}};
  }
  return canvas_bridge_->Submit(handle, commands,
                                [this] { return IsActive(); });
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
  if (handle.kind != WebPythonTargetKind::kElement ||
      handle.document != document_id_ || handle.generation != generation_ ||
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
       !element->HasTagName(html_names::kSpanTag) &&
       !element->HasTagName(html_names::kCodeTag) &&
       !element->HasTagName(html_names::kPreTag) &&
       !element->HasTagName(html_names::kOutputTag) &&
       !element->HasTagName(html_names::kButtonTag) &&
       !element->HasTagName(html_names::kLiTag) &&
       !element->HasTagName(html_names::kH1Tag) &&
       !element->HasTagName(html_names::kH2Tag) &&
       !element->HasTagName(html_names::kH3Tag)) ||
      element->GetCustomElementState() != CustomElementState::kUncustomized ||
      element->FastHasAttribute(html_names::kIsAttr) ||
      element->firstElementChild()) {
    return {{},
            WebPythonDomError::kNotAllowed,
            "set_text requires an approved native text element without element "
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

WebPythonControlStateResult PythonScriptController::GetControlState(
    WebPythonDomHandle handle) {
  auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return {status};
  }
  auto* element = ResolveElement(handle);
  if (!element) {
    return {{{}, WebPythonDomError::kReference, "Expired control handle"}};
  }
  if (!IsPythonUiElement(element)) {
    return {
        {{}, WebPythonDomError::kNotAllowed, "Expected a native UI element"}};
  }
  String value;
  bool checked = false;
  if (auto* input = DynamicTo<HTMLInputElement>(element)) {
    if (input->type() == "checkbox") {
      checked = input->Checked();
    } else if (IsPythonValueInput(input)) {
      value = input->Value();
    } else {
      return {{{}, WebPythonDomError::kNotAllowed, "Unsupported input type"}};
    }
  } else if (auto* textarea = DynamicTo<HTMLTextAreaElement>(element)) {
    value = textarea->Value();
  } else if (auto* select = DynamicTo<HTMLSelectElement>(element)) {
    value = select->Value();
  }
  if (value.length() > 16384 || value.Utf8().size() > 16384) {
    return {
        {{}, WebPythonDomError::kLimit, "Control value exceeds 16 KiB UTF-8"}};
  }
  auto* control = DynamicTo<HTMLFormControlElement>(element);
  auto* dialog = DynamicTo<HTMLDialogElement>(element);
  return {{},
          value,
          checked,
          element->FastHasAttribute(html_names::kHiddenAttr),
          control && control->IsDisabledFormControl(),
          dialog && dialog->IsOpen()};
}

WebPythonDomResult PythonScriptController::UpdateElement(
    WebPythonDomHandle handle,
    const WebPythonElementUpdate& update) {
  auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return status;
  }
  auto* element = ResolveElement(handle);
  if (!element) {
    return {{}, WebPythonDomError::kReference, "Expired UI handle"};
  }
  if (!IsPythonUiElement(element)) {
    return {{}, WebPythonDomError::kNotAllowed, "Expected a native UI element"};
  }
  if (update.name.length() > 64 || update.name.Utf8().size() > 64 ||
      update.value.length() > 16384 || update.value.Utf8().size() > 16384) {
    return {{}, WebPythonDomError::kLimit, "UI string limit exceeded"};
  }
  auto denied = [] {
    return WebPythonDomResult{{},
                              WebPythonDomError::kNotAllowed,
                              "Unsupported UI operation or attribute"};
  };
  auto* input = DynamicTo<HTMLInputElement>(element);
  switch (update.action) {
    case WebPythonElementAction::kValue:
      if (IsPythonValueInput(input)) {
        input->SetValue(update.value);
      } else if (auto* textarea = DynamicTo<HTMLTextAreaElement>(element)) {
        textarea->SetValue(update.value);
      } else if (auto* select = DynamicTo<HTMLSelectElement>(element)) {
        select->SelectOptionByValue(update.value);
      } else {
        return denied();
      }
      break;
    case WebPythonElementAction::kChecked:
      if (!input || input->type() != "checkbox") {
        return denied();
      }
      input->SetChecked(update.enabled);
      break;
    case WebPythonElementAction::kHidden:
      element->SetBooleanAttribute(html_names::kHiddenAttr, update.enabled);
      break;
    case WebPythonElementAction::kDisabled:
      if (!IsA<HTMLFormControlElement>(element)) {
        return denied();
      }
      element->SetBooleanAttribute(html_names::kDisabledAttr, update.enabled);
      break;
    case WebPythonElementAction::kClass: {
      const auto token = update.name.Utf8();
      if (token.empty() || token.find_first_not_of(
                               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTU"
                               "VWXYZ0123456789_-") != std::string::npos) {
        return denied();
      }
      if (update.enabled &&
          !element->classList().contains(AtomicString(update.name)) &&
          element->classList().length() >= 64) {
        return {{}, WebPythonDomError::kLimit, "At most 64 UI classes"};
      }
      if (update.enabled) {
        element->classList().Add(AtomicString(update.name));
      } else {
        element->classList().Remove(AtomicString(update.name));
      }
      break;
    }
    case WebPythonElementAction::kAttribute: {
      const QualifiedName* attr = nullptr;
      for (const auto* candidate :
           {&html_names::kAriaLabelAttr, &html_names::kAriaSelectedAttr,
            &html_names::kAriaExpandedAttr, &html_names::kAriaPressedAttr,
            &html_names::kAriaValuenowAttr, &html_names::kTitleAttr}) {
        if (String(update.name) == candidate->LocalName()) {
          attr = candidate;
        }
      }
      if (!attr) {
        return denied();
      }
      element->setAttribute(*attr, AtomicString(update.value));
      break;
    }
    case WebPythonElementAction::kTabIndex:
      if (update.number != -1 && update.number != 0) {
        return denied();
      }
      element->setTabIndex(static_cast<int>(update.number));
      break;
    case WebPythonElementAction::kProgress:
      if (!element->HasTagName(html_names::kDivTag) ||
          !std::isfinite(update.number) || update.number < 0 ||
          update.number > 100) {
        return denied();
      }
      element->SetInlineStyleProperty(CSSPropertyID::kWidth, update.number,
                                      CSSPrimitiveValue::UnitType::kPercentage);
      break;
    case WebPythonElementAction::kFocus: {
      auto* options = FocusOptions::Create();
      options->setPreventScroll(true);
      element->Focus(options);
      break;
    }
    case WebPythonElementAction::kShowModal:
    case WebPythonElementAction::kCloseDialog: {
      auto* dialog = DynamicTo<HTMLDialogElement>(element);
      if (!dialog) {
        return denied();
      }
      ExceptionState exception(nullptr);
      if (update.action == WebPythonElementAction::kShowModal) {
        dialog->showModal(exception);
      } else {
        dialog->close();
      }
      if (exception.HadException()) {
        return {{}, WebPythonDomError::kState, "Dialog operation failed"};
      }
      break;
    }
    case WebPythonElementAction::kScrollIntoView: {
      auto* options = ScrollIntoViewOptions::Create();
      auto* media = MediaValues::CreateDynamicIfFrameExists(
          GetSupplementable()->GetFrame());
      options->setBehavior(media && media->PrefersReducedMotion()
                               ? V8ScrollBehavior::Enum::kInstant
                               : V8ScrollBehavior::Enum::kSmooth);
      options->setBlock(V8ScrollLogicalPosition::Enum::kNearest);
      element->scrollIntoViewWithOptions(options);
      break;
    }
    default:
      return denied();
  }
  return IsActive()
             ? WebPythonDomResult{}
             : WebPythonDomResult{{},
                                  WebPythonDomError::kReference,
                                  "Document changed during UI operation"};
}

WebPythonDomResult PythonScriptController::AddEventListener(
    WebPythonDomHandle handle,
    const WebString& type,
    WebPythonCallbackHandle callback,
    bool once,
    bool passive,
    bool capture) {
  auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return status;
  }
  EventTarget* target = handle.kind == WebPythonTargetKind::kElement
                            ? ResolveElement(handle)
                            : ResolveTarget(handle);
  if (!target) {
    return {
        {}, WebPythonDomError::kReference, "Invalid or expired event target"};
  }
  if (auto* element = DynamicTo<Element>(target->ToNode());
      element &&
      (!element->IsHTMLElement() ||
       element->GetCustomElementState() != CustomElementState::kUncustomized ||
       element->FastHasAttribute(html_names::kIsAttr))) {
    return {{},
            WebPythonDomError::kNotAllowed,
            "Python listeners require an ordinary HTML element"};
  }
  const String event_type = type;
  const bool supported =
      handle.kind == WebPythonTargetKind::kDocument
          ? (event_type == "keydown" || event_type == "keyup" ||
             event_type == "visibilitychange" || event_type == "focusin")
      : handle.kind == WebPythonTargetKind::kWindow
          ? (event_type == "blur" || event_type == "focus" ||
             event_type == "resize")
          : (event_type == "keydown" || event_type == "keyup" ||
             event_type == "close" || event_type == "cancel" ||
             event_type == "click" || event_type == "input" ||
             event_type == "change" || event_type == "pointerdown" ||
             event_type == "pointerup" || event_type == "pointermove" ||
             event_type == "pointercancel" ||
             event_type == "gotpointercapture" ||
             event_type == "lostpointercapture");
  if (!supported) {
    return {{},
            WebPythonDomError::kNotAllowed,
            "Unsupported event type for target"};
  }
  if ((event_type == "close" || event_type == "cancel") &&
      (!target->ToNode() || !IsA<HTMLDialogElement>(target->ToNode()))) {
    return {
        {}, WebPythonDomError::kNotAllowed, "Expected a native dialog target"};
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
  auto* listener = MakeGarbageCollected<PythonEventListener>(
      *this, *target, *current_source_, handle, callback, AtomicString(type),
      once, passive, capture);
  listeners_.push_back(listener);
  // Each registration has a distinct native listener, preserving Blink's
  // dispatch order and add/remove-during-dispatch semantics. once is consumed
  // only after Python admission, rather than by EventTarget before admission.
  AddEventListenerOptionsResolved options;
  options.SetCapture(capture);
  options.SetPassive(passive);
  options.SetPassiveSpecified(true);
  target->addEventListener(listener->type, listener, options);
  return {};
}

WebPythonDomResult PythonScriptController::RemoveEventListener(
    WebPythonDomHandle handle,
    WebPythonCallbackHandle callback) {
  auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return status;
  }
  // Removal only revokes a registration. Preserve cancellation after a
  // target is detached or changes policy, without resolving a live Node.
  const bool valid_id = handle.kind == WebPythonTargetKind::kElement
                            ? handle.id && handle.id <= handles_.size()
                            : (handle.kind == WebPythonTargetKind::kDocument ||
                               handle.kind == WebPythonTargetKind::kWindow) &&
                                  handle.id == 1;
  if (handle.document != document_id_ || handle.generation != generation_ ||
      !valid_id) {
    return {
        {}, WebPythonDomError::kReference, "Invalid or expired event target"};
  }
  RevokeEventListener(handle, callback);
  return {};
}

void PythonScriptController::DetachListener(PythonEventListener& listener) {
  listener.active = false;
  if (listener.target) {
    listener.target->removeEventListener(listener.type, &listener,
                                         listener.capture);
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
        listener->handle.kind == handle.kind &&
        listener->callback.context == callback.context &&
        listener->callback.id == callback.id) {
      DetachListener(*listener);
      return;
    }
  }
}

Element* PythonScriptController::ResolveElement(
    WebPythonDomHandle handle) const {
  if (handle.kind != WebPythonTargetKind::kElement ||
      handle.document != document_id_ || handle.generation != generation_ ||
      !handle.id || handle.id > handles_.size()) {
    return nullptr;
  }
  Element* element = handles_[static_cast<wtf_size_t>(handle.id - 1)]->element;
  return element && &element->GetDocument() == GetSupplementable() &&
                 element->isConnected() && !element->IsInShadowTree()
             ? element
             : nullptr;
}

EventTarget* PythonScriptController::ResolveTarget(
    WebPythonDomHandle handle) const {
  if (handle.kind == WebPythonTargetKind::kElement) {
    Element* e = ResolveElement(handle);
    return e && e->IsHTMLElement() &&
                   e->GetCustomElementState() ==
                       CustomElementState::kUncustomized &&
                   !e->FastHasAttribute(html_names::kIsAttr)
               ? e
               : nullptr;
  }
  if (handle.document != document_id_ || handle.generation != generation_ ||
      handle.id != 1) {
    return nullptr;
  }
  if (handle.kind == WebPythonTargetKind::kDocument) {
    return GetSupplementable();
  }
  if (handle.kind == WebPythonTargetKind::kWindow) {
    return GetSupplementable()->domWindow();
  }
  return nullptr;
}

WebPythonDomResult PythonScriptController::GetEventTarget(
    WebPythonTargetKind kind) {
  auto result = CheckDomCall();
  if (result.error != WebPythonDomError::kNone) {
    return result;
  }
  if (kind != WebPythonTargetKind::kDocument &&
      kind != WebPythonTargetKind::kWindow) {
    return {{},
            WebPythonDomError::kNotAllowed,
            "Expected document or window target"};
  }
  return {{document_id_, generation_, 1, kind}};
}

WebPythonPageStateResult PythonScriptController::GetPageState() {
  auto result = CheckDomCall();
  if (result.error != WebPythonDomError::kNone) {
    return {result, {}};
  }
  auto& document = *GetSupplementable();
  auto* frame = document.GetFrame();
  auto* view = document.View();
  auto size = view ? view->Size() : gfx::Size();
  // Do not force layout during a Python frame or initial parsing.
  // Report the viewport currently published by the native frame view.
  auto* media = MediaValues::CreateDynamicIfFrameExists(document.GetFrame());
  return {{},
          {document.hidden(), document.hasFocus(),
           AdjustForAbsoluteZoom::AdjustInt(size.width(),
                                            frame->LayoutZoomFactor()),
           AdjustForAbsoluteZoom::AdjustInt(size.height(),
                                            frame->LayoutZoomFactor()),
           media && media->PrefersReducedMotion(), input_epoch_}};
}

WebPythonDomResult PythonScriptController::ControlEvent(
    uint64_t token,
    WebPythonEventAction action) {
  auto result = CheckDomCall();
  if (result.error != WebPythonDomError::kNone) {
    return result;
  }
  if (!active_event_ || !token || token != active_event_token_) {
    return {{},
            WebPythonDomError::kReference,
            "The synchronous event control token has expired"};
  }
  switch (action) {
    case WebPythonEventAction::kPreventDefault:
      if (active_event_passive_) {
        return {{},
                WebPythonDomError::kNotAllowed,
                "A passive listener cannot prevent default"};
      }
      if (active_event_->cancelable()) {
        active_event_->preventDefault();
      }
      break;
    case WebPythonEventAction::kStopPropagation:
      active_event_->stopPropagation();
      break;
    case WebPythonEventAction::kStopImmediatePropagation:
      active_event_->stopImmediatePropagation();
      break;
  }
  return {};
}

WebPythonDomResult PythonScriptController::SetPointerCapture(
    WebPythonDomHandle handle,
    int32_t id,
    bool capture) {
  auto result = CheckDomCall();
  if (result.error != WebPythonDomError::kNone) {
    return result;
  }
  auto* element = ResolveElement(handle);
  if (!element || ResolveTarget(handle) != element) {
    return {
        {}, WebPythonDomError::kReference, "Invalid pointer capture element"};
  }
  for (wtf_size_t i = pointer_captures_.size(); i > 0; --i) {
    const auto& entry = pointer_captures_[i - 1];
    if (!entry->element || !entry->element->hasPointerCapture(entry->id)) {
      pointer_captures_.EraseAt(i - 1);
    }
  }
  if (capture && pointer_captures_.size() >= 32) {
    return {
        {}, WebPythonDomError::kLimit, "At most 32 Python pointer captures"};
  }
  ExceptionState exception(nullptr);
  if (capture) {
    element->setPointerCapture(id, exception);
  } else {
    element->releasePointerCapture(id, exception);
  }
  if (exception.HadException()) {
    return {{},
            WebPythonDomError::kState,
            "Pointer capture requires an active pointer and connected element"};
  }
  for (wtf_size_t i = pointer_captures_.size(); i > 0; --i) {
    const auto& entry = pointer_captures_[i - 1];
    if (entry->id == id && (capture || entry->element == element)) {
      pointer_captures_.EraseAt(i - 1);
    }
  }
  if (capture && element->hasPointerCapture(id)) {
    pointer_captures_.push_back(
        MakeGarbageCollected<PythonPointerCapture>(*element, id));
  }
  return {};
}

void PythonScriptController::ReleasePointerCaptures() {
  for (const auto& entry : pointer_captures_) {
    if (entry->element && entry->element->hasPointerCapture(entry->id)) {
      ExceptionState exception(nullptr);
      entry->element->releasePointerCapture(entry->id, exception);
    }
  }
  pointer_captures_.clear();
}

void PythonScriptController::ResetInput() {
  ++input_epoch_;
  if (browser_bridge_) {
    browser_bridge_->Silence();
  }
  ReleasePointerCaptures();
}

WebPythonDomResult PythonScriptController::RequestFrame(
    WebPythonCallbackHandle callback) {
  auto status = CheckDomCall();
  if (status.error != WebPythonDomError::kNone) {
    return status;
  }
  if (!current_source_ || !callback.context || !callback.id) {
    return {{}, WebPythonDomError::kState, "No active Python frame source"};
  }
  if (pending_frame_) {
    return {{},
            WebPythonDomError::kState,
            "At most one pending Python animation frame per document"};
  }
  ScheduleFrame(callback, *current_source_);
  return {};
}

WebPythonDomResult PythonScriptController::CancelFrame(
    WebPythonCallbackHandle callback) {
  auto status = CheckDomCall();
  if (status.error == WebPythonDomError::kNone) {
    RevokeFrame(callback);
  }
  return status;
}

void PythonScriptController::RevokeFrame(WebPythonCallbackHandle callback) {
  if (pending_frame_ && pending_frame_->callback.context == callback.context &&
      pending_frame_->callback.id == callback.id) {
    if (pending_frame_->Id()) {
      GetSupplementable()->CancelAnimationFrame(pending_frame_->Id(),
                                                FrameCallbackType::kWebExposed);
    }
    pending_frame_ = nullptr;
  }
}

void PythonScriptController::ScheduleFrame(WebPythonCallbackHandle callback,
                                           PythonScriptSource& source) {
  // Use a fresh adapter on retry: the previous Invoke's AsyncTask probe is
  // still on the stack. Blink swaps its callback list before invoking it, so
  // this request can only run in a later rendering update.
  pending_frame_ =
      MakeGarbageCollected<PythonAnimationFrame>(*this, source, callback);
  GetSupplementable()->RequestAnimationFrame(pending_frame_,
                                             FrameCallbackType::kWebExposed);
}

void PythonScriptController::RetryFrame(WebPythonCallbackHandle callback) {
  if (stopped_ || !pending_frame_ || pending_frame_->Id() ||
      pending_frame_->callback.context != callback.context ||
      pending_frame_->callback.id != callback.id) {
    return;
  }
  auto* thread = ThreadState::Current();
  if (ScriptForbiddenScope::IsScriptForbidden() ||
      ScriptForbiddenScope::WillBeScriptForbidden() ||
      thread->IsSweepingOnOwningThread() ||
      cppgc::subtle::HeapState::IsInAtomicPause(thread->heap_handle())) {
    cleanup_task_runner_->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&PythonScriptController::RetryFrame,
                       WrapWeakPersistent(this), callback),
        base::Milliseconds(1));
    return;
  }
  ScheduleFrame(callback, *pending_frame_->source);
}

void PythonScriptController::DispatchFrame(PythonAnimationFrame& frame,
                                           double timestamp_ms) {
  if (stopped_ || !context_ || pending_frame_ != &frame) {
    return;
  }
  auto* thread = ThreadState::Current();
  if (thread->IsSweepingOnOwningThread() ||
      cppgc::subtle::HeapState::IsInAtomicPause(thread->heap_handle())) {
    // Do not allocate a new GC object or enter Python during GC. Re-arm only
    // from a safe task, then let the normal native frame scheduler dispatch.
    cleanup_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&PythonScriptController::RetryFrame,
                                  WrapWeakPersistent(this), frame.callback));
    return;
  }
  Document& document = *GetSupplementable();
  if (!Eligible(document, false) || !ScriptsAllowed(document)) {
    Dispose();
    return;
  }
  if (document.hidden() || g_python_running || running_ ||
      ScriptForbiddenScope::IsScriptForbidden() ||
      ScriptForbiddenScope::WillBeScriptForbidden()) {
    ScheduleFrame(frame.callback, *frame.source);
    return;
  }
  Persistent<PythonScriptController> protect(this);
  Persistent<PythonScriptSource> source(frame.source);
  if (!g_runner->AllowsUnrestrictedPages() &&
      !document.domWindow()->GetContentSecurityPolicy()->AllowInline(
          ContentSecurityPolicy::InlineType::kScript, nullptr, source->source,
          source->nonce, source->url, source->position)) {
    Dispose();
    return;
  }
  if (!context_->CanInvokeFrameCallback()) {
    ScheduleFrame(frame.callback, *source);
    return;
  }
  base::AutoReset<bool> entry(&g_python_running, true);
  base::AutoReset<bool> callback_entry(&callback_running_, true);
  callback_dom_calls_ = 0;
  if (canvas_bridge_) {
    canvas_bridge_->BeginExecution();
  }
  running_ = true;
  current_source_ = source;
  // Consume before entry, allowing exactly one new request from this callback.
  pending_frame_ = nullptr;
  const auto result =
      context_->InvokeFrameCallback(frame.callback, timestamp_ms, *this);
  if (result.callback_skipped && !stopped_ && Eligible(document, false)) {
    ScheduleFrame(frame.callback, *source);
  }
  FinishCallback(
      *source, result,
      StrCat({"[pyscript frame ", String::Number(frame.callback.id), "] "}));
}

void PythonScriptController::DispatchEvent(PythonEventListener& listener,
                                           Event& event) {
  if (stopped_ || !listener.active || !context_ ||
      event.type() != listener.type) {
    return;
  }
  const auto epoch_before = input_epoch_;
  bool delivered = false;
  // A skipped input invalidates Python's held-key/pointer state. No old event
  // is queued; a later frame can observe the epoch even when keyup was lost.
  const bool input = event.type() != event_type_names::kClick &&
                     event.type() != event_type_names::kResize &&
                     event.type() != event_type_names::kFocus &&
                     event.type() != event_type_names::kFocusin &&
                     event.type() != event_type_names::kClose &&
                     event.type() != event_type_names::kCancel;
  // Native UI operations may synchronously notify focus/modal listeners while
  // Python is already running. Skipping those notifications loses no held
  // key/pointer transition and must not invalidate the caller's input epoch.
  absl::Cleanup invalidate = [&] {
    if (input && !delivered && input_epoch_ == epoch_before) {
      ++input_epoch_;
    }
  };
  auto* target = ResolveTarget(listener.handle);
  if (!target || target != listener.target || event.currentTarget() != target) {
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
    // Never queue a skipped event. Keep diagnostics bounded for this document.
    if (!dispatch_warning_reported_) {
      dispatch_warning_reported_ = true;
      Log(document, *listener.source,
          "[pyscript] Python event skipped: document "
          "policy or execution state does not allow callbacks.",
          mojom::blink::ConsoleMessageLevel::kWarning);
    }
    return;
  }
  Persistent<PythonScriptController> protect(this);
  Persistent<PythonEventListener> protect_listener(&listener);
  Persistent<PythonScriptSource> source(listener.source);
  if (!g_runner->AllowsUnrestrictedPages() &&
      !document.domWindow()->GetContentSecurityPolicy()->AllowInline(
          ContentSecurityPolicy::InlineType::kScript, nullptr, source->source,
          source->nonce, source->url, source->position)) {
    return;
  }
  if (!context_->CanInvokeCallback()) {
    const auto now = base::TimeTicks::Now();
    if (last_window_warning_.is_null() ||
        now - last_window_warning_ >= base::Seconds(1)) {
      last_window_warning_ = now;
      Log(document, *source,
          "[pyscript] Python event skipped: event budget window is full.",
          mojom::blink::ConsoleMessageLevel::kWarning);
    }
    return;
  }
  base::AutoReset<bool> entry(&g_python_running, true);
  base::AutoReset<bool> callback_entry(&callback_running_, true);
  callback_dom_calls_ = 0;
  if (canvas_bridge_) {
    canvas_bridge_->BeginExecution();
  }
  running_ = true;
  current_source_ = source;
  WebPythonCallbackEvent snapshot;
  snapshot.type = event.type();
  snapshot.current_target = listener.handle;
  snapshot.is_trusted = event.isTrusted();
  snapshot.cancelable = event.cancelable();
  snapshot.bubbles = event.bubbles();
  snapshot.default_prevented = event.defaultPrevented();
  snapshot.passive = listener.passive;
  snapshot.input_epoch = input_epoch_;
  auto bounded = [](const String& text) {
    return WebString::FromUtf8(
        base::TruncateUTF8ToByteSize(text.substr(0, 16384).Utf8(), 16384));
  };
  if (auto* node = event.target() ? event.target()->ToNode() : nullptr;
      node && &node->GetDocument() == &document) {
    snapshot.target_is_editable = IsEditable(*node);
    for (Element* e = node->IsElementNode() ? To<Element>(node)
                                            : node->parentElement();
         e; e = e->parentElement()) {
      if (IsA<HTMLInputElement>(e) || IsA<HTMLTextAreaElement>(e) ||
          IsA<HTMLSelectElement>(e) || e->GetShadowRoot() ||
          e->HasTagName(html_names::kButtonTag) ||
          (e->HasTagName(html_names::kATag) &&
           e->FastHasAttribute(html_names::kHrefAttr))) {
        snapshot.target_is_editable = true;
      }
      if (auto* dialog = DynamicTo<HTMLDialogElement>(e);
          dialog && dialog->IsOpen()) {
        snapshot.target_in_dialog = true;
      }
    }
    // Form data is present only on form notifications, never on key/click.
    if (event.type() == event_type_names::kInput ||
        event.type() == event_type_names::kChange) {
      String value;
      if (auto* input_element = DynamicTo<HTMLInputElement>(node)) {
        value = input_element->Value();
        snapshot.checked = input_element->Checked();
      } else if (auto* textarea = DynamicTo<HTMLTextAreaElement>(node)) {
        value = textarea->Value();
      } else if (auto* select = DynamicTo<HTMLSelectElement>(node)) {
        value = select->Value();
      }
      snapshot.value = bounded(value);
      snapshot.value_truncated = snapshot.value.length() < value.length();
    }
  }
  if (auto* key = DynamicTo<KeyboardEvent>(event)) {
    snapshot.key = bounded(key->key());
    snapshot.code = bounded(key->code());
    snapshot.repeat = key->repeat();
    snapshot.is_composing = key->isComposing();
    snapshot.alt_key = key->altKey();
    snapshot.ctrl_key = key->ctrlKey();
    snapshot.meta_key = key->metaKey();
    snapshot.shift_key = key->shiftKey();
  }
  if (auto* pointer = DynamicTo<PointerEvent>(event)) {
    snapshot.pointer_id = pointer->pointerId();
    snapshot.pointer_type = bounded(pointer->pointerType());
    snapshot.button = pointer->button();
    snapshot.buttons = pointer->buttons();
    snapshot.client_x = pointer->clientX();
    snapshot.client_y = pointer->clientY();
    snapshot.is_primary = pointer->isPrimary();
    snapshot.alt_key = pointer->altKey();
    snapshot.ctrl_key = pointer->ctrlKey();
    snapshot.meta_key = pointer->metaKey();
    snapshot.shift_key = pointer->shiftKey();
    if (!std::isfinite(snapshot.client_x) ||
        !std::isfinite(snapshot.client_y)) {
      running_ = false;
      current_source_ = nullptr;
      return;
    }
  }
  if (listener.once) {
    DetachListener(listener);
  }
  CHECK(++next_event_token_);
  snapshot.control_token = active_event_token_ = next_event_token_;
  active_event_ = &event;
  active_event_passive_ = listener.passive;
  const auto result =
      context_->InvokeCallback(listener.callback, snapshot, *this);
  // Revoke before diagnostics or releasing any Python-owned callback data.
  active_event_ = nullptr;
  active_event_token_ = 0;
  delivered = !result.callback_skipped;
  FinishCallback(*source, result,
                 StrCat({"[pyscript callback ",
                         String::Number(listener.callback.id), "] "}));
}

void PythonScriptController::FinishCallback(const PythonScriptSource& source,
                                            const WebPythonScriptResult& result,
                                            const String& prefix) {
  Document& document = *GetSupplementable();
  if (stopped_ || !Eligible(document, false)) {
    // Invalidation closes page output as well as DOM capability. Disposal of
    // the interpreter still waits until the synchronous Python stack returns.
    Dispose();
    running_ = false;
    ReleaseContext();
    return;
  }
  if (!result.stdout_text.IsEmpty()) {
    AppendConsole(source, StrCat({prefix, result.stdout_text}),
                  mojom::blink::ConsoleMessageLevel::kInfo);
  }
  if (!result.stderr_text.IsEmpty()) {
    AppendConsole(source, StrCat({prefix, result.stderr_text}),
                  mojom::blink::ConsoleMessageLevel::kError);
  }
  if (!result.success && !result.callback_skipped) {
    AppendConsole(source, StrCat({prefix, result.error}),
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
  controller->input_reset_listener_ =
      MakeGarbageCollected<PythonInputResetListener>(*controller);
  document.addEventListener(event_type_names::kVisibilitychange,
                            controller->input_reset_listener_, true);
  document.domWindow()->addEventListener(
      event_type_names::kBlur, controller->input_reset_listener_, true);
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
