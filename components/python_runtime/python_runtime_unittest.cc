// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/python_runtime/python_runtime.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace native_python {
namespace {

TEST(PythonRuntimeTest, CalculationUnicodeAndPersistentGlobals) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  EXPECT_TRUE(context->Execute("answer=6*7\n").success);
  auto result = context->Execute("print('Python 世界', answer)\n");
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ("Python 世界 42\n", result.stdout_text);
}

TEST(PythonRuntimeTest, ContextsSeparateGlobalsModulesAndOutput) {
  std::string error;
  auto first = PythonContext::Create(&error);
  auto second = PythonContext::Create(&error);
  ASSERT_TRUE(first && second) << error;
  EXPECT_TRUE(first->Execute("import sys\nx=42\nsys.marker=42\nprint('first')")
                  .success);
  auto result = second->Execute(
      "import sys\nassert 'x' not in globals()\nassert not "
      "hasattr(sys,'marker')\nprint('second')");
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ("second\n", result.stdout_text);
}

TEST(PythonRuntimeTest, RuntimeAndSyntaxErrorsHaveSourceAndRecover) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  auto runtime = context->Execute(
      "def f():\n    raise ValueError('failure')\nf()", "demo.py");
  EXPECT_FALSE(runtime.success);
  EXPECT_NE(std::string::npos, runtime.error.find("ValueError: failure"));
  EXPECT_NE(std::string::npos, runtime.error.find("demo.py, line 2"));
  auto syntax = context->Execute("if :\n", "syntax.py");
  EXPECT_FALSE(syntax.success);
  EXPECT_NE(std::string::npos, syntax.error.find("SyntaxError"));
  EXPECT_NE(std::string::npos, syntax.error.find("syntax.py"));
  EXPECT_TRUE(context->Execute("print(42)").success);
}

TEST(PythonRuntimeTest, OutputIsBoundedAndStderrIsSeparate) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  auto result = context->Execute(
      "import sys\nprint('err', file=sys.stderr)\nprint('界'*20000)");
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_TRUE(result.output_truncated);
  EXPECT_LE(result.stdout_text.size() + result.stderr_text.size(), 16384u);
  EXPECT_EQ("err\n", result.stderr_text);
  EXPECT_EQ("42\n", context->Execute("print(42)").stdout_text);
}

TEST(PythonRuntimeTest, PathsEnvironmentAndDynamicModulesAreExcluded) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  auto result = context->Execute(
      "import sys, _imp\nassert not sys.path\nassert sys.flags.isolated\n"
      "assert sys.flags.ignore_environment\nassert not sys.flags.no_site == 0\n"
      "assert 'site' not in sys.modules\nassert not _imp.extension_suffixes()\n"
      "import encodings.utf_8\nassert '世界'.encode('utf-8').decode('utf-8') "
      "== '世界'\n"
      "try:\n    import pathlib\nexcept ModuleNotFoundError:\n    "
      "pass\nelse:\n    raise AssertionError('disk stdlib was found')\n");
  EXPECT_TRUE(result.success) << result.error;
}

TEST(PythonRuntimeTest, SourceBoundariesAreRejectedWithoutPoisoningContext) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  EXPECT_FALSE(context->Execute(std::string(65537, ' ')).success);
  EXPECT_FALSE(context->Execute(std::string("a\0b", 3)).success);
  EXPECT_FALSE(context->Execute(std::string(1, '\xff')).success);
  EXPECT_TRUE(context->Execute("assert 1+1 == 2").success);
}

TEST(PythonRuntimeTest, RepeatedDestructionAndOutOfOrderContexts) {
  std::string error;
  auto first = PythonContext::Create(&error);
  auto survivor = PythonContext::Create(&error);
  ASSERT_TRUE(first && survivor) << error;
  first.reset();
  for (int i = 0; i < 20; ++i) {
    auto context = PythonContext::Create(&error);
    ASSERT_TRUE(context) << error;
    EXPECT_TRUE(
        context
            ->Execute("import atexit\natexit.register(print, 'finalizing')\n"
                      "class Cycle:\n    def __del__(self):\n        "
                      "print('cycle finalizing')\nx=Cycle()\nx.self=x\n")
            .success);
  }
  EXPECT_TRUE(survivor->Execute("assert 6*7 == 42").success);
}

TEST(PythonRuntimeTest, SubinterpretersCannotCreateThreads) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  auto result = context->Execute(
      "import _thread\ntry:\n    _thread.start_new_thread(lambda: None, ())\n"
      "except RuntimeError:\n    pass\nelse:\n    "
      "raise AssertionError('thread creation was enabled')\n");
  EXPECT_TRUE(result.success) << result.error;
}

TEST(PythonRuntimeTest, FixedDiagnosticSelfTest) {
  auto report = RunSelfTest();
  EXPECT_EQ(true, report.FindBool("passed")) << report.DebugString();
  EXPECT_EQ("3.13.18", *report.FindString("version"));
}

}  // namespace
}  // namespace native_python
