// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_RENDERER_PYTHON_DOCUMENT_CONTEXT_H_
#define CHROME_RENDERER_PYTHON_DOCUMENT_CONTEXT_H_

#include <memory>

#include "third_party/blink/public/web/web_python_script_runner.h"

// Main-thread synchronous bridge. The caller owns document policy and lifetime.
std::unique_ptr<blink::WebPythonScriptContext> CreatePythonDocumentContext();

#endif  // CHROME_RENDERER_PYTHON_DOCUMENT_CONTEXT_H_
