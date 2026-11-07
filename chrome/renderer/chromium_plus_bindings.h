// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_RENDERER_CHROMIUM_PLUS_BINDINGS_H_
#define CHROME_RENDERER_CHROMIUM_PLUS_BINDINGS_H_

#include "v8/include/v8-forward.h"

namespace chromium_plus {

// Installs the default chromiumPlus version on a newly created global object.
// Must be called on the context's owning thread before author scripts run.
void InstallBindings(v8::Local<v8::Context> context);

}  // namespace chromium_plus

#endif  // CHROME_RENDERER_CHROMIUM_PLUS_BINDINGS_H_
