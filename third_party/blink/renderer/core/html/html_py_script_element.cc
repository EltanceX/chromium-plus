// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/html_py_script_element.h"

#include "third_party/blink/renderer/core/dom/attribute.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/frame/csp/content_security_policy.h"
#include "third_party/blink/renderer/core/html/python_script_controller.h"

namespace blink {

HTMLPyScriptElement::HTMLPyScriptElement(Document& document)
    : HTMLElement(html_names::kPyscriptTag, document) {}

void HTMLPyScriptElement::Trace(Visitor* visitor) const {
  visitor->Trace(parser_document_);
  HTMLElement::Trace(visitor);
}

void HTMLPyScriptElement::ChildrenChanged(const ChildrenChange& change) {
  HTMLElement::ChildrenChanged(change);
  changed_by_api_ |= !change.ByParser();
}

void HTMLPyScriptElement::BeginParserBlock(const TextPosition& position,
                                           bool original_document_input) {
  parser_document_ = &GetDocument();
  position_ = position;
  eligible_input_ = original_document_input;
}

bool HTMLPyScriptElement::HasUnsupportedAttributes() const {
  return unsupported_attributes_ || FastHasAttribute(html_names::kSrcAttr) ||
         FastHasAttribute(html_names::kAsyncAttr) ||
         FastHasAttribute(html_names::kDeferAttr) ||
         FastHasAttribute(html_names::kTypeAttr);
}

void HTMLPyScriptElement::EndParserBlock(bool explicit_end_tag) {
  if (completed_) {
    return;
  }
  completed_ = true;
  if (!eligible_input_ || parser_document_ != &GetDocument() ||
      changed_by_api_) {
    return;
  }
  if (!explicit_end_tag) {
    PythonScriptController::Report(
        GetDocument(), *this, "Missing </pyscript>; block was not executed.");
    return;
  }
  unsupported_attributes_ = HasUnsupportedAttributes();
  if (unsupported_attributes_) {
    PythonScriptController::Report(
        GetDocument(), *this,
        "Python blocks support inline source only; src, async, defer and type "
        "attributes are not supported.");
    return;
  }
  source_ = textContent();
  if (source_.Utf8().size() > 64 * 1024) {
    source_ = String();
    PythonScriptController::Report(GetDocument(), *this,
                                   "Python source exceeds 64 KiB.");
    return;
  }
  if (ContentSecurityPolicy::IsNonceableElement(this)) {
    bool nonceable = true;
    for (const Attribute& attribute : Attributes()) {
      if (attribute.LocalName().FindIgnoringAsciiCase("<pyscript") !=
              kNotFound ||
          attribute.Value().FindIgnoringAsciiCase("<pyscript") != kNotFound) {
        nonceable = false;
        break;
      }
    }
    if (nonceable) {
      nonce_ = nonce();
    }
  }
  PythonScriptController::Register(*this);
}

}  // namespace blink
