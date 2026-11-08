// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/python_script_controller.h"

#include "base/check.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/web/web_python_script_runner.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/document_parser.h"
#include "third_party/blink/renderer/core/frame/csp/content_security_policy.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/html/html_py_script_element.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/platform/bindings/script_forbidden_scope.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
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

namespace {

WebPythonScriptRunner* g_runner = nullptr;

bool Eligible(Document& document) {
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
         !window->IsContextDestroyed() && parser &&
         parser->HasPythonScriptEligibleInput() &&
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
    : Supplement<Document>(document) {}

void PythonScriptController::Trace(Visitor* visitor) const {
  visitor->Trace(pending_);
  Supplement<Document>::Trace(visitor);
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
  if (controller->consumed_) {
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
  if (!controller || controller->consumed_) {
    return;
  }
  controller->consumed_ = true;
  HeapVector<Member<PendingPythonScriptBlock>> pending;
  pending.swap(controller->pending_);
  if (ScriptForbiddenScope::IsScriptForbidden() || !Eligible(document)) {
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
  const auto results = g_runner->Run(sources);
  if (!Eligible(document)) {
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
