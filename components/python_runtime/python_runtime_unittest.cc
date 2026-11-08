// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/python_runtime/python_runtime.h"

#include <functional>

#include "base/test/simple_test_tick_clock.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace native_python {
namespace {

class TestDomHost final : public DomHost {
 public:
  bool IsActive() const override { return active; }
  DomResult GetElementById(std::string_view id) override {
    ++gets;
    if (on_get) {
      on_get();
    }
    return id == "missing" ? DomResult{} : DomResult{{7, 1, 3}};
  }
  DomResult SetText(DomHandle handle, std::string_view text) override {
    ++sets;
    if (handle.document != 7 || handle.generation != 1 || handle.id != 3) {
      return {{}, DomError::kReference, "Stale handle"};
    }
    if (failure != DomError::kNone) {
      return {{}, failure, "Rejected DOM target"};
    }
    value = text;
    return {};
  }
  bool active = true;
  int gets = 0;
  int sets = 0;
  DomError failure = DomError::kNone;
  std::string value;
  std::function<void()> on_get;
};

TEST(PythonRuntimeTest, DomModuleUsesOpaqueHandlesAndStrictStrings) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  TestDomHost host;
  auto result = context->Execute(R"PY(
import dom
assert dom.get_element_by_id('missing') is None
handle = dom.get_element_by_id('result')
assert type(handle) is dom.ElementHandle
dom.set_text(handle, '世界 <img src=x> &amp;')
for make in (lambda: dom.ElementHandle(),
             lambda: object.__new__(dom.ElementHandle),
             lambda: dom.set_text(3, 'forged'),
             lambda: dom.set_text(None, 'forged'),
             lambda: dom.set_text(handle, 42),
             lambda: dom.get_element_by_id(42)):
    try:
        make()
    except TypeError:
        pass
    else:
        raise AssertionError('implicit conversion or handle fabrication')
)PY",
                                 "dom.py", &host);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ("世界 <img src=x> &amp;", host.value);
  EXPECT_EQ(1, host.sets);
}

TEST(PythonRuntimeTest, DomHandlesPersistAndModulesAreInterpreterLocal) {
  std::string error;
  auto first = PythonContext::Create(&error);
  auto second = PythonContext::Create(&error);
  ASSERT_TRUE(first && second) << error;
  TestDomHost host;
  EXPECT_TRUE(first
                  ->Execute("import dom\nh=dom.get_element_by_id('result')\n"
                            "dom.marker=42",
                            "first", &host)
                  .success);
  auto result =
      first->Execute("dom.set_text(h, 'second block')", "first", &host);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ("second block", host.value);
  result = second->Execute(
      "import dom\nassert not hasattr(dom, 'marker')\n"
      "assert 'h' not in globals()",
      "second", &host);
  EXPECT_TRUE(result.success) << result.error;
}

TEST(PythonRuntimeTest, DomRejectsInactiveHostsAndConvertsHostErrors) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  TestDomHost host;
  auto result = context->Execute("import dom\ndom.get_element_by_id('x')");
  EXPECT_FALSE(result.success);
  EXPECT_NE(std::string::npos, result.error.find("ReferenceError"));
  host.active = false;
  result = context->Execute("dom.get_element_by_id('x')", "inactive", &host);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(0, host.gets);
  EXPECT_EQ(StopReason::kInactive, result.stop_reason);
  host.active = true;
  EXPECT_FALSE(context->Execute("print('must not resume')").success);
  context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  for (auto [failure, name] :
       {std::pair{DomError::kReference, "ReferenceError"},
        std::pair{DomError::kNotAllowed, "PermissionError"},
        std::pair{DomError::kLimit, "ValueError"},
        std::pair{DomError::kState, "RuntimeError"}}) {
    host.failure = failure;
    result = context->Execute(
        "import dom\n"
        "dom.set_text(dom.get_element_by_id('x'),'text')",
        "host-error", &host);
    EXPECT_FALSE(result.success);
    EXPECT_NE(std::string::npos, result.error.find(name));
  }
}

TEST(PythonRuntimeTest, DomUtf8BoundsAndSurrogatesDoNotReachHost) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  TestDomHost host;
  auto result = context->Execute(R"PY(
import dom
h = dom.get_element_by_id('x')
for text in ('x' * 16385, '界' * 5462):
    for call in (lambda: dom.set_text(h, text),
                 lambda: dom.get_element_by_id(text)):
        try:
            call()
        except ValueError:
            pass
        else:
            raise AssertionError('unbounded string')
try:
    dom.set_text(h, '\ud800')
except UnicodeEncodeError:
    pass
else:
    raise AssertionError('invalid Unicode')
dom.set_text(h, '界' * 5461 + 'x')
)PY",
                                 "limits", &host);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ(1, host.gets);
  EXPECT_EQ(1, host.sets);
  EXPECT_EQ(16384u, host.value.size());
}

TEST(PythonRuntimeTest, DomCapabilityIsRevokedBeforeDiagnosticsAndFinalizers) {
  std::string error;
  TestDomHost host;
  {
    auto context = PythonContext::Create(&error);
    ASSERT_TRUE(context) << error;
    auto result = context->Execute(R"PY(
import dom, atexit
class Finalizer:
    def __del__(self):
        try:
            dom.get_element_by_id('finalizer')
        except ReferenceError:
            pass
value = Finalizer()
value.cycle = value
atexit.register(lambda: dom.get_element_by_id('atexit'))
class Failure(Exception):
    def __str__(self):
        try:
            dom.get_element_by_id('diagnostic')
        except ReferenceError:
            return 'capability revoked'
        return 'unexpected access'
raise Failure()
)PY",
                                   "cleanup", &host);
    EXPECT_FALSE(result.success);
    EXPECT_NE(std::string::npos, result.error.find("capability revoked"));
  }
  EXPECT_EQ(0, host.gets);
}

TEST(PythonRuntimeTest, NestedEntryFromDomHostIsRejectedWithoutGilDeadlock) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  TestDomHost host;
  host.on_get = [&] {
    EXPECT_FALSE(context->Execute("print('nested')").success);
    EXPECT_FALSE(PythonContext::Create(&error));
  };
  auto result = context->Execute("import dom\ndom.get_element_by_id('x')",
                                 "nested", &host);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_TRUE(context->Execute("assert 6 * 7 == 42").success);
}

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
  EXPECT_FALSE(result.success);
  EXPECT_EQ(StopReason::kOutput, result.stop_reason);
  EXPECT_TRUE(result.output_truncated);
  EXPECT_LE(result.stdout_text.size() + result.stderr_text.size(), 16384u);
  EXPECT_EQ("err\n", result.stderr_text);
  EXPECT_TRUE(context->Execute("print(42)").stdout_text.empty());
}

TEST(PythonRuntimeTest, CooperativeBudgetInterruptsPurePythonAndLatchesStop) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  const auto started = base::TimeTicks::Now();
  auto result = context->Execute("while True: pass", "loop.py");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(StopReason::kBlockTime, result.stop_reason);
  EXPECT_NE(std::string::npos, result.error.find("100 ms"));
  EXPECT_LT(base::TimeTicks::Now() - started, base::Seconds(5));
  result = context->Execute("print('must not run')");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(StopReason::kBlockTime, result.stop_reason);
  EXPECT_TRUE(result.stdout_text.empty());
}

TEST(PythonRuntimeTest, DocumentTimeIsCumulativeButIdleTimeIsExcluded) {
  std::string error;
  base::SimpleTestTickClock clock;
  auto context = PythonContext::CreateForTesting(&error, &clock);
  ASSERT_TRUE(context) << error;
  TestDomHost host;
  host.on_get = [&] { clock.Advance(base::Milliseconds(80)); };
  for (int i = 0; i < 3; ++i) {
    auto result = context->Execute("import dom\ndom.get_element_by_id('x')",
                                   "cumulative", &host);
    EXPECT_TRUE(result.success) << result.error;
    clock.Advance(base::Seconds(10));
  }
  auto result = context->Execute("dom.get_element_by_id('x')", "last", &host);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(StopReason::kDocumentTime, result.stop_reason);
  EXPECT_EQ(4, host.gets);
  EXPECT_EQ(StopReason::kDocumentTime,
            context->Execute("print('must not run')").stop_reason);
}

TEST(PythonRuntimeTest, OutputAndDiagnosticsShareTheDocumentBudget) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  EXPECT_TRUE(context->Execute("print('x' * 9000)").success);
  auto result = context->Execute("import sys\nsys.stderr.write('界' * 3000)");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(StopReason::kOutput, result.stop_reason);
  EXPECT_TRUE(result.output_truncated);
  EXPECT_LE(result.stderr_text.size(), 16384u - 9001);
  EXPECT_EQ(result.stderr_text.size() % 3, 0u);
  EXPECT_TRUE(context->Execute("print('forbidden')").stdout_text.empty());

  context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  EXPECT_TRUE(context->Execute("print('x' * 15000)").success);
  result = context->Execute("raise ValueError('y' * 3000)");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(StopReason::kOutput, result.stop_reason);

  context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  result = context->Execute("raise ValueError('界' * 10000)");
  EXPECT_EQ(StopReason::kOutput, result.stop_reason);
  EXPECT_TRUE(result.output_truncated);
  EXPECT_EQ(StopReason::kOutput, context->Execute("pass").stop_reason);
}

TEST(PythonRuntimeTest, CatchingOutputFailureCannotRestoreDomOrLaterExecution) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  TestDomHost host;
  auto result = context->Execute(R"PY(
import sys, dom
h = dom.get_element_by_id('x')
# This intentionally demonstrates that tracing is not a language sandbox.
# The C++ output latch must still revoke DOM and reject the next Execute.
sys.settrace(None)
try:
    print('x' * 20000)
except BaseException:
    pass
try:
    dom.set_text(h, 'forbidden')
except ReferenceError:
    pass
)PY",
                                 "caught-output", &host);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(StopReason::kOutput, result.stop_reason);
  EXPECT_EQ(0, host.sets);
  EXPECT_EQ(StopReason::kOutput, context->Execute("pass").stop_reason);
}

TEST(PythonRuntimeTest, DomBudgetAndNavigationStopAtNativeBoundary) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  TestDomHost host;
  host.failure = DomError::kStopped;
  auto result = context->Execute(
      "import dom\ndom.set_text(dom.get_element_by_id('x'), 'denied')",
      "dom-budget", &host);
  EXPECT_EQ(StopReason::kDomCalls, result.stop_reason);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(StopReason::kDomCalls, context->Execute("pass").stop_reason);

  context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  host.failure = DomError::kNone;
  host.on_get = [&] { host.active = false; };
  result = context->Execute(
      "import dom\ndom.set_text(dom.get_element_by_id('x'), 'denied')",
      "navigation", &host);
  EXPECT_EQ(StopReason::kInactive, result.stop_reason);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(1, host.sets);
}

TEST(PythonRuntimeTest, ExceptionFormattingCannotRunAnUnboundedPythonLoop) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  auto result = context->Execute(R"PY(
class Failure(Exception):
    def __str__(self):
        while True:
            pass
raise Failure()
)PY",
                                 "diagnostic-loop");
  EXPECT_FALSE(result.success);
  EXPECT_EQ(StopReason::kBlockTime, result.stop_reason);
}

TEST(PythonRuntimeTest, CleanupCooperativelyInterruptsAPlainPythonFinalizer) {
  const auto before = GetRuntimeStatisticsForTesting();
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  ASSERT_TRUE(context
                  ->Execute(R"PY(
class Finalizer:
    def __del__(self):
        while True:
            pass
value = Finalizer()
value.cycle = value
)PY")
                  .success);
  const auto started = base::TimeTicks::Now();
  context.reset();
  EXPECT_LT(base::TimeTicks::Now() - started, base::Seconds(5));
  const auto after = GetRuntimeStatisticsForTesting();
  EXPECT_EQ(before.live_contexts, after.live_contexts);
  EXPECT_EQ(before.cleanup_budget_exhaustions + 1,
            after.cleanup_budget_exhaustions);
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
  const auto before = GetRuntimeStatisticsForTesting();
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
  EXPECT_EQ(before.live_contexts + 1,
            GetRuntimeStatisticsForTesting().live_contexts);
  survivor.reset();
  EXPECT_EQ(before.live_contexts,
            GetRuntimeStatisticsForTesting().live_contexts);
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
