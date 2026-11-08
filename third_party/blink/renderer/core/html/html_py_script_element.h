// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_HTML_HTML_PY_SCRIPT_ELEMENT_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_HTML_HTML_PY_SCRIPT_ELEMENT_H_

#include "third_party/blink/renderer/core/html/html_element.h"
#include "third_party/blink/renderer/platform/wtf/text/text_position.h"

namespace blink {

// Uses HTMLElement's JS interface; there is no executable binding API.
class HTMLPyScriptElement final : public HTMLElement {
 public:
  explicit HTMLPyScriptElement(Document&);
  ElementType GetElementType() const final {
    return ElementType::kHTMLPyScriptElement;
  }
  void Trace(Visitor*) const override;

  // Called only by the tree builder, never by insertion/mutation callbacks.
  void BeginParserBlock(const TextPosition&, bool original_document_input);
  void EndParserBlock(bool explicit_end_tag);

  const String& SourceSnapshot() const { return source_; }
  const AtomicString& NonceSnapshot() const { return nonce_; }
  const TextPosition& SourcePosition() const { return position_; }
  bool HasUnsupportedAttributes() const;

 private:
  void ChildrenChanged(const ChildrenChange&) override;

  WeakMember<Document> parser_document_;
  TextPosition position_ = TextPosition::MinimumPosition();
  String source_;
  AtomicString nonce_;
  bool eligible_input_ = false;
  bool completed_ = false;
  bool changed_by_api_ = false;
  bool unsupported_attributes_ = false;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_HTML_HTML_PY_SCRIPT_ELEMENT_H_
