// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_RENDERER_PYTHON_SCRIPT_RUNNER_H_
#define CHROME_RENDERER_PYTHON_SCRIPT_RUNNER_H_

// Install the main-thread bridge, whose origin policy remains deny-by-default.
void InitializePythonScriptRunner();

#endif  // CHROME_RENDERER_PYTHON_SCRIPT_RUNNER_H_
