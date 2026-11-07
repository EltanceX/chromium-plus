// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/chromium_plus_bindings.h"

#include "v8/include/v8-context.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"

namespace chromium_plus {

namespace {

constexpr char kVersion[] = "0.0.1";

}  // namespace

void InstallBindings(v8::Local<v8::Context> context) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope handle_scope(isolate);
  v8::MicrotasksScope microtasks_scope(
      isolate, context->GetMicrotaskQueue(),
      v8::MicrotasksScope::kDoNotRunMicrotasks);
  v8::Context::Scope context_scope(context);

  context->Global()
      ->CreateDataProperty(
          context, v8::String::NewFromUtf8Literal(isolate, "chromiumPlus"),
          v8::String::NewFromUtf8Literal(isolate, kVersion))
      .Check();
}

}  // namespace chromium_plus
