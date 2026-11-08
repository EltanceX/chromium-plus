// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PYTHON_SCRIPT_CONTROLLER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PYTHON_SCRIPT_CONTROLLER_H_

#include "third_party/blink/renderer/platform/heap/collection_support/heap_vector.h"
#include "third_party/blink/renderer/platform/supplementable.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class Document;
class HTMLPyScriptElement;
class PendingPythonScriptBlock;

class PythonScriptController final
    : public GarbageCollected<PythonScriptController>,
      public Supplement<Document> {
 public:
  static const char kSupplementName[];
  explicit PythonScriptController(Document&);
  static void Register(HTMLPyScriptElement&);
  static void ExecutePending(Document&);
  static void Report(Document&, const HTMLPyScriptElement&, const String&);
  void Trace(Visitor*) const override;

 private:
  HeapVector<Member<PendingPythonScriptBlock>> pending_;
  bool consumed_ = false;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_HTML_PYTHON_SCRIPT_CONTROLLER_H_
