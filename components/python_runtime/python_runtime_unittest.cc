// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/python_runtime/python_runtime.h"

#include <functional>
#include <vector>

#include "base/memory/raw_ref.h"
#include "base/test/simple_test_tick_clock.h"
#include "base/threading/platform_thread.h"
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

CallbackEvent Click(bool trusted = false) {
  return {"click", {7, 1, 3}, trusted};
}

TEST(PythonRuntimeTest, CallbacksCaptureClosuresAndUseReadOnlyEventSnapshots) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  ASSERT_TRUE(context
                  ->Execute(R"PY(
import dom
def make_callback():
    count = 0
    def callback(event):
        nonlocal count
        count += 1
        assert type(event) is dom.CallbackEvent
        assert type(event.current_target) is dom.ElementHandle
        assert event.type == 'click'
        for name, value in [('type', 'other'), ('current_target', None),
                            ('is_trusted', True), ('extra', 42)]:
            try:
                setattr(event, name, value)
            except (AttributeError, TypeError):
                pass
            else:
                raise AssertionError('mutable event')
        for make in (lambda: dom.CallbackEvent(),
                     lambda: object.__new__(dom.CallbackEvent)):
            try:
                make()
            except TypeError:
                pass
            else:
                raise AssertionError('fabricated event')
        global saved_event
        saved_event = event
        dom.set_text(event.current_target, str(count))
        print(count, event.is_trusted)
    return callback
callback = make_callback()
)PY",
                            "events.py")
                  .success);
  const auto registration = context->RegisterCallback("callback");
  ASSERT_TRUE(registration.result.success) << registration.result.error;
  ASSERT_NE(0u, registration.handle.id);
  ASSERT_TRUE(context->Execute("callback = lambda event: print('replacement')")
                  .success);
  TestDomHost host;
  auto result =
      context->InvokeCallback(registration.handle, Click(true), &host);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ("1 True\n", result.stdout_text);
  EXPECT_EQ("1", host.value);
  result = context->InvokeCallback(registration.handle, Click(), &host);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ("2 False\n", result.stdout_text);
  EXPECT_EQ("2", host.value);
  result = context->Execute(
      "assert saved_event.type == 'click'\n"
      "assert saved_event.is_trusted is False\n"
      "dom.set_text(saved_event.current_target, 'saved')",
      "saved", &host);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ("saved", host.value);
}

TEST(PythonRuntimeTest, CallbackHandlesRejectOtherAndDestroyedContexts) {
  std::string error;
  auto first = PythonContext::Create(&error);
  auto second = PythonContext::Create(&error);
  ASSERT_TRUE(first && second) << error;
  ASSERT_TRUE(
      first->Execute("callback = lambda event: print('first')").success);
  ASSERT_TRUE(
      second->Execute("callback = lambda event: print('second')").success);
  const auto one = first->RegisterCallback("callback");
  const auto two = second->RegisterCallback("callback");
  ASSERT_TRUE(one.result.success && two.result.success);
  EXPECT_EQ(one.handle.id, two.handle.id);
  EXPECT_NE(one.handle.context, two.handle.context);
  auto result = second->InvokeCallback(one.handle, Click());
  EXPECT_FALSE(result.success);
  EXPECT_NE(std::string::npos, result.error.find("ReferenceError"));
  EXPECT_FALSE(second->RemoveCallback(one.handle).success);
  EXPECT_EQ("second\n",
            second->InvokeCallback(two.handle, Click()).stdout_text);
  first.reset();
  auto replacement = PythonContext::Create(&error);
  ASSERT_TRUE(replacement) << error;
  ASSERT_TRUE(replacement->Execute("callback = lambda event: None").success);
  const auto three = replacement->RegisterCallback("callback");
  ASSERT_TRUE(three.result.success);
  EXPECT_NE(one.handle.context, three.handle.context);
  EXPECT_FALSE(replacement->InvokeCallback(one.handle, Click()).success);
  EXPECT_TRUE(replacement->InvokeCallback(three.handle, Click()).success);
}

TEST(PythonRuntimeTest, CallbackRegistryIsBoundedAndNeverReusesRemovedIds) {
  std::string error;
  base::SimpleTestTickClock clock;
  auto context = PythonContext::CreateForTesting(&error, &clock);
  ASSERT_TRUE(context) << error;
  ASSERT_TRUE(context->Execute("callback = lambda event: None").success);
  std::vector<CallbackHandle> handles;
  for (unsigned i = 0; i < 128; ++i) {
    const auto registration = context->RegisterCallback("callback");
    ASSERT_TRUE(registration.result.success) << registration.result.error;
    handles.push_back(registration.handle);
  }
  const auto full = context->RegisterCallback("callback");
  EXPECT_FALSE(full.result.success);
  EXPECT_EQ(0u, full.handle.id);
  EXPECT_NE(std::string::npos, full.result.error.find("128"));
  EXPECT_TRUE(context->RemoveCallback(handles.front()).success);
  EXPECT_TRUE(context->RemoveCallback(handles.front()).success);
  const auto replacement = context->RegisterCallback("callback");
  ASSERT_TRUE(replacement.result.success);
  EXPECT_GT(replacement.handle.id, handles.back().id);
  EXPECT_FALSE(context->InvokeCallback(handles.front(), Click()).success);
  EXPECT_TRUE(context->InvokeCallback(replacement.handle, Click()).success);
  EXPECT_FALSE(context->RemoveCallback({}).success);
}

TEST(PythonRuntimeTest, CallbackRegistrationValidatesNamesAndSyncCallables) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  ASSERT_TRUE(context
                  ->Execute(R"PY(
not_callable = 42
async def coroutine(event):
    pass
def generator(event):
    yield 42
async def async_generator(event):
    yield 42
class Handler:
    async def method(self, event):
        pass
bound = Handler().method
callback = lambda event: None
)PY")
                  .success);
  for (auto name :
       {"not_callable", "coroutine", "generator", "async_generator", "bound"}) {
    const auto registration = context->RegisterCallback(name);
    EXPECT_FALSE(registration.result.success) << name;
    EXPECT_EQ(0u, registration.handle.id);
    EXPECT_NE(std::string::npos, registration.result.error.find("TypeError"));
  }
  for (const auto& name : {std::string(), std::string(2049, 'a'),
                           std::string("a\0b", 3), std::string(1, '\xff')}) {
    EXPECT_FALSE(context->RegisterCallback(name).result.success);
  }
  EXPECT_NE(
      std::string::npos,
      context->RegisterCallback("callback()").result.error.find("NameError"));
  const auto valid = context->RegisterCallback("callback");
  ASSERT_TRUE(valid.result.success);
  auto invalid_event = Click();
  invalid_event.type = "keydown";
  EXPECT_FALSE(context->InvokeCallback(valid.handle, invalid_event).success);
  invalid_event = Click();
  invalid_event.current_target = {};
  EXPECT_FALSE(context->InvokeCallback(valid.handle, invalid_event).success);
  EXPECT_TRUE(context->InvokeCallback(valid.handle, Click()).success);
}

TEST(PythonRuntimeTest,
     CallbackRejectsDeferredResultsWithoutRunningTheirBodies) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  ASSERT_TRUE(context
                  ->Execute(R"PY(
ran = False
async def coroutine(event):
    global ran
    ran = True
def generator(event):
    global ran
    ran = True
    yield 42
async def async_generator(event):
    global ran
    ran = True
    yield 42
class AsyncCallable:
    async def __call__(self, event):
        global ran
        ran = True
factory_coro = lambda event: coroutine(event)
factory_gen = lambda event: generator(event)
factory_async_gen = lambda event: async_generator(event)
async_callable = AsyncCallable()
)PY")
                  .success);
  for (auto name :
       {"factory_coro", "factory_gen", "factory_async_gen", "async_callable"}) {
    const auto registration = context->RegisterCallback(name);
    ASSERT_TRUE(registration.result.success) << registration.result.error;
    const auto result = context->InvokeCallback(registration.handle, Click());
    EXPECT_FALSE(result.success) << name;
    EXPECT_NE(std::string::npos, result.error.find("TypeError"));
    EXPECT_EQ(StopReason::kNone, result.stop_reason);
    EXPECT_TRUE(result.stdout_text.empty());
    EXPECT_EQ(std::string::npos, result.stderr_text.find("never awaited"));
  }
  EXPECT_TRUE(context->Execute("assert not ran").success);
}

TEST(PythonRuntimeTest, CallbackExceptionsKeepTracebacksAndPermitLaterCalls) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  ASSERT_TRUE(context
                  ->Execute(R"PY(
count = 0
def callback(event):
    global count
    count += 1
    if count == 1:
        raise ValueError('callback failure')
    print(count)
)PY",
                            "click-source.py")
                  .success);
  const auto registration = context->RegisterCallback("callback");
  ASSERT_TRUE(registration.result.success);
  auto result = context->InvokeCallback(registration.handle, Click());
  EXPECT_FALSE(result.success);
  EXPECT_EQ(StopReason::kNone, result.stop_reason);
  EXPECT_NE(std::string::npos,
            result.error.find("ValueError: callback failure"));
  EXPECT_NE(std::string::npos, result.error.find("click-source.py"));
  result = context->InvokeCallback(registration.handle, Click());
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ("2\n", result.stdout_text);
}

TEST(PythonRuntimeTest, RemovingCurrentCallbackRetainsItUntilDomIsRevoked) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  ASSERT_TRUE(context
                  ->Execute(R"PY(
import dom
finalized = False
class Callback:
    def __call__(self, event):
        dom.get_element_by_id('remove-me')
        assert not finalized
        print('alive')
    def __del__(self):
        global finalized
        finalized = True
        try:
            dom.get_element_by_id('must-not-reach-host')
        except ReferenceError:
            print('revoked')
callback = Callback()
)PY")
                  .success);
  const auto registration = context->RegisterCallback("callback");
  ASSERT_TRUE(registration.result.success);
  ASSERT_TRUE(context->Execute("del callback").success);
  TestDomHost host;
  host.on_get = [&] {
    EXPECT_TRUE(context->RemoveCallback(registration.handle).success);
    EXPECT_TRUE(context->RemoveCallback(registration.handle).success);
  };
  const auto result =
      context->InvokeCallback(registration.handle, Click(), &host);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ("alive\nrevoked\n", result.stdout_text);
  EXPECT_EQ(1, host.gets);
  EXPECT_TRUE(context->Execute("assert finalized").success);
  EXPECT_FALSE(context->InvokeCallback(registration.handle, Click()).success);
}

TEST(PythonRuntimeTest, CallbackResultAndExceptionCleanupCannotUseDom) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  ASSERT_TRUE(context
                  ->Execute(R"PY(
import dom
def check_revoked():
    try:
        dom.get_element_by_id('forbidden')
    except ReferenceError:
        return 'revoked'
    raise AssertionError('DOM escaped execution')
class Result:
    def __del__(self):
        print(check_revoked())
def returning(event):
    return Result()
class Failure(Exception):
    def __str__(self):
        return check_revoked()
def failing(event):
    raise Failure()
def suspended():
    try:
        yield 42
    finally:
        print(check_revoked())
suspension = suspended()
next(suspension)
def returning_generator(event):
    return suspension
)PY")
                  .success);
  TestDomHost host;
  const auto returning = context->RegisterCallback("returning");
  ASSERT_TRUE(returning.result.success);
  auto result = context->InvokeCallback(returning.handle, Click(), &host);
  EXPECT_TRUE(result.success) << result.error;
  EXPECT_EQ("revoked\n", result.stdout_text);
  const auto failing = context->RegisterCallback("failing");
  ASSERT_TRUE(failing.result.success);
  result = context->InvokeCallback(failing.handle, Click(), &host);
  EXPECT_FALSE(result.success);
  EXPECT_NE(std::string::npos, result.error.find("Failure: revoked"));
  const auto generator = context->RegisterCallback("returning_generator");
  ASSERT_TRUE(generator.result.success);
  result = context->InvokeCallback(generator.handle, Click(), &host);
  EXPECT_FALSE(result.success);
  EXPECT_EQ("revoked\n", result.stdout_text);
  EXPECT_EQ(0, host.gets);
}

TEST(PythonRuntimeTest, CallbackEntriesShareReentryAndInactiveHostChecks) {
  std::string error;
  auto context = PythonContext::Create(&error);
  auto other = PythonContext::Create(&error);
  ASSERT_TRUE(context && other) << error;
  ASSERT_TRUE(
      context
          ->Execute(
              "import dom\ncallback = lambda event: dom.get_element_by_id('x')")
          .success);
  ASSERT_TRUE(other->Execute("callback = lambda event: None").success);
  const auto registration = context->RegisterCallback("callback");
  const auto foreign = other->RegisterCallback("callback");
  ASSERT_TRUE(registration.result.success && foreign.result.success);
  TestDomHost host;
  host.on_get = [&] {
    EXPECT_FALSE(context->InvokeCallback(registration.handle, Click()).success);
    EXPECT_FALSE(context->RegisterCallback("callback").result.success);
    EXPECT_FALSE(other->RemoveCallback(foreign.handle).success);
    EXPECT_FALSE(PythonContext::Create(&error));
  };
  EXPECT_TRUE(
      context->InvokeCallback(registration.handle, Click(), &host).success);
  host.active = false;
  const auto result =
      context->InvokeCallback(registration.handle, Click(), &host);
  EXPECT_EQ(StopReason::kInactive, result.stop_reason);
  EXPECT_EQ(1, host.gets);
  EXPECT_FALSE(context->RemoveCallback(registration.handle).success);
  EXPECT_TRUE(other->InvokeCallback(foreign.handle, Click()).success);
}

TEST(PythonRuntimeTest,
     CallbackBudgetStopsLoopsAndCannotBeResetByOtherEntries) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  ASSERT_TRUE(
      context->Execute("def callback(event):\n    while True: pass").success);
  const auto registration = context->RegisterCallback("callback");
  ASSERT_TRUE(registration.result.success);
  const auto started = base::TimeTicks::Now();
  const auto result = context->InvokeCallback(registration.handle, Click());
  EXPECT_FALSE(result.success);
  EXPECT_EQ(StopReason::kBlockTime, result.stop_reason);
  EXPECT_LT(base::TimeTicks::Now() - started, base::Seconds(5));
  EXPECT_EQ(StopReason::kBlockTime, context->Execute("pass").stop_reason);
  EXPECT_EQ(StopReason::kBlockTime,
            context->InvokeCallback(registration.handle, Click()).stop_reason);
  EXPECT_EQ(StopReason::kBlockTime,
            context->RemoveCallback(registration.handle).stop_reason);
  const auto stopped = context->RegisterCallback("callback");
  EXPECT_EQ(0u, stopped.handle.id);
  EXPECT_EQ(StopReason::kBlockTime, stopped.result.stop_reason);
}

TEST(PythonRuntimeTest,
     CallbackAndSourceShareCumulativeBudgetsUntilEventPhaseFour) {
  std::string error;
  base::SimpleTestTickClock clock;
  auto context = PythonContext::CreateForTesting(&error, &clock);
  ASSERT_TRUE(context) << error;
  ASSERT_TRUE(
      context
          ->Execute(
              "import dom\ncallback = lambda event: dom.get_element_by_id('x')")
          .success);
  const auto registration = context->RegisterCallback("callback");
  ASSERT_TRUE(registration.result.success);
  TestDomHost host;
  host.on_get = [&] { clock.Advance(base::Milliseconds(80)); };
  EXPECT_TRUE(
      context->InvokeCallback(registration.handle, Click(), &host).success);
  clock.Advance(base::Seconds(10));
  EXPECT_TRUE(
      context->Execute("dom.get_element_by_id('x')", "source", &host).success);
  EXPECT_TRUE(
      context->InvokeCallback(registration.handle, Click(), &host).success);
  EXPECT_EQ(
      StopReason::kDocumentTime,
      context->InvokeCallback(registration.handle, Click(), &host).stop_reason);
  EXPECT_EQ(4, host.gets);

  context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  ASSERT_TRUE(
      context->Execute("callback = lambda event: print('界' * 1000)").success);
  const auto output_callback = context->RegisterCallback("callback");
  ASSERT_TRUE(output_callback.result.success);
  ASSERT_TRUE(context->Execute("print('x' * 15000)").success);
  const auto output = context->InvokeCallback(output_callback.handle, Click());
  EXPECT_EQ(StopReason::kOutput, output.stop_reason);
  EXPECT_TRUE(output.output_truncated);
  EXPECT_LE(output.stdout_text.size(), 16384u - 15001);
}

TEST(PythonRuntimeTest, CallbackCyclesParticipateInGcAndTeardownRevokesDom) {
  const auto before = GetRuntimeStatisticsForTesting();
  std::string error;
  TestDomHost host;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  ASSERT_TRUE(context
                  ->Execute(R"PY(
import dom, gc
finalized = 0
class Callback:
    def __call__(self, event):
        self.event = event
    def __del__(self):
        global finalized
        finalized += 1
        try:
            dom.get_element_by_id('forbidden')
        except ReferenceError:
            pass
callback = Callback()
callback.cycle = callback
callback.module = dom
)PY")
                  .success);
  const auto registration = context->RegisterCallback("callback");
  ASSERT_TRUE(registration.result.success);
  ASSERT_TRUE(
      context->InvokeCallback(registration.handle, Click(), &host).success);
  ASSERT_TRUE(
      context->Execute("del callback\ngc.collect()\nassert finalized == 0")
          .success);
  ASSERT_TRUE(context->RemoveCallback(registration.handle).success);
  EXPECT_TRUE(context->Execute("gc.collect()\nassert finalized == 1").success);
  ASSERT_TRUE(
      context->Execute("callback = Callback()\ncallback.cycle = callback")
          .success);
  ASSERT_TRUE(context->RegisterCallback("callback").result.success);
  ASSERT_TRUE(context->Execute("del callback").success);
  context.reset();
  EXPECT_EQ(0, host.gets);
  EXPECT_EQ(before.live_contexts,
            GetRuntimeStatisticsForTesting().live_contexts);
}

TEST(PythonRuntimeDeathTest,
     CallbackEntryRejectsOtherThreadsBeforeUsingPython) {
  std::string error;
  auto context = PythonContext::Create(&error);
  ASSERT_TRUE(context) << error;
  class WrongThread final : public base::PlatformThread::Delegate {
   public:
    explicit WrongThread(PythonContext& context) : context_(context) {}
    void ThreadMain() override { context_->InvokeCallback({}, Click()); }

   private:
    const raw_ref<PythonContext> context_;
  };
  EXPECT_DEATH_IF_SUPPORTED(
      {
        WrongThread delegate(*context);
        base::PlatformThreadHandle thread;
        ASSERT_TRUE(base::PlatformThread::Create(0, &delegate, &thread));
        base::PlatformThread::Join(thread);
      },
      "thread_");
}

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
