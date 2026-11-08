// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PYTHON_RUNTIME_PYTHON_RUNTIME_H_
#define COMPONENTS_PYTHON_RUNTIME_PYTHON_RUNTIME_H_

#include <memory>
#include <string>
#include <string_view>

#include "base/feature_list.h"
#include "base/values.h"

// This target is always a DLL, including non-component builds.
#if defined(IS_PYTHON_RUNTIME_IMPL)
#define PYTHON_RUNTIME_EXPORT __declspec(dllexport)
#else
#define PYTHON_RUNTIME_EXPORT __declspec(dllimport)
#endif

namespace native_python {

PYTHON_RUNTIME_EXPORT
BASE_DECLARE_FEATURE(kNativePythonRuntimeSelfTest);

struct PYTHON_RUNTIME_EXPORT ExecutionResult {
  bool success = false;
  bool output_truncated = false;
  std::string stdout_text;
  std::string stderr_text;
  std::string error;
};

// A single, main-thread-owned subinterpreter. The embedder must check page
// execution policy before use. All instances and the process runtime must use
// the same thread.
class PYTHON_RUNTIME_EXPORT PythonContext {
 public:
  static std::unique_ptr<PythonContext> Create(std::string* error);
  ~PythonContext();
  PythonContext(const PythonContext&) = delete;
  PythonContext& operator=(const PythonContext&) = delete;
  ExecutionResult Execute(std::string_view source,
                          std::string_view filename = "<native-python>");

 private:
  struct Impl;
  explicit PythonContext(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

PYTHON_RUNTIME_EXPORT bool IsInitialized();
PYTHON_RUNTIME_EXPORT std::string Version();
// Fixed diagnostic code only; result can be logged after sandbox engagement.
PYTHON_RUNTIME_EXPORT base::DictValue RunSelfTest();

}  // namespace native_python

#endif  // COMPONENTS_PYTHON_RUNTIME_PYTHON_RUNTIME_H_
