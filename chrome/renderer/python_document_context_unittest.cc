// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/python_document_context.h"

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace {
using blink::WebPythonCallbackEvent;
using blink::WebPythonCallbackHandle;
using blink::WebPythonDomError;
using blink::WebPythonDomHandle;
using blink::WebPythonDomHost;
using blink::WebPythonDomResult;
using blink::WebPythonScriptContext;
using blink::WebPythonScriptResult;
using blink::WebString;

class TestHost final : public WebPythonDomHost {
 public:
  bool IsActive() const override { return active; }
  WebPythonDomResult GetElementById(const WebString& id) override {
    return id.Utf8() == "missing" ? WebPythonDomResult{}
                                  : WebPythonDomResult{target};
  }
  WebPythonDomResult SetText(WebPythonDomHandle element,
                             const WebString& text) override {
    CheckTarget(element);
    value = text.Utf8();
    return {};
  }
  WebPythonDomResult AddEventListener(WebPythonDomHandle element,
                                      const WebString& type,
                                      WebPythonCallbackHandle callback,
                                      bool once) override {
    ++adds;
    CheckTarget(element);
    EXPECT_EQ("click", type.Utf8());
    EXPECT_NE(0u, callback.context);
    EXPECT_NE(0u, callback.id);
    last = callback;
    last_once = once;
    installed.push_back(callback);
    if (on_add) {
      on_add();
    }
    return {{}, failure, WebString::FromUtf8("host listener failure")};
  }
  WebPythonDomResult RemoveEventListener(
      WebPythonDomHandle element,
      WebPythonCallbackHandle callback) override {
    ++removes;
    CheckTarget(element);
    Erase(callback);
    return {};
  }
  void RevokeEventListener(WebPythonDomHandle element,
                           WebPythonCallbackHandle callback) override {
    ++revokes;
    CheckTarget(element);
    Erase(callback);
  }
  WebPythonCallbackEvent Event(bool trusted = false) const {
    return {WebString::FromUtf8("click"), target, trusted};
  }
  void CheckTarget(WebPythonDomHandle element) {
    EXPECT_EQ(target.document, element.document);
    EXPECT_EQ(target.generation, element.generation);
    EXPECT_EQ(target.id, element.id);
  }
  void Erase(WebPythonCallbackHandle callback) {
    std::erase_if(installed, [&](auto item) {
      return item.context == callback.context && item.id == callback.id;
    });
  }
  bool active = true;
  bool last_once = false;
  WebPythonDomHandle target{71, 9, 2};
  WebPythonCallbackHandle last;
  WebPythonDomError failure = WebPythonDomError::kNone;
  std::vector<WebPythonCallbackHandle> installed;
  int adds = 0;
  int removes = 0;
  int revokes = 0;
  std::string value;
  std::function<void()> on_add;
};

WebPythonScriptResult RunSource(WebPythonScriptContext& context,
                                TestHost& host,
                                std::string_view source) {
  unsigned reported = 0;
  auto results = context.Run(
      {{WebString::FromUtf8(source), WebString::FromUtf8("adapter.py")}}, host,
      [&](const WebPythonScriptResult&) {
        ++reported;
        return true;
      });
  EXPECT_EQ(1u, reported);
  EXPECT_EQ(1u, results.size());
  return results.empty() ? WebPythonScriptResult{} : results.front();
}

constexpr std::string_view kRegister = R"PY(
import dom
count = 0
def callback(event):
    global count
    count += 1
    assert event.type == 'click'
    dom.set_text(event.current_target, 'value-' + str(count))
    print(count, event.is_trusted)
listener = dom.add_event_listener(dom.get_element_by_id('button'), 'click', callback, once=False)
)PY";

TEST(PythonDocumentContextTest,
     ForwardsListenerAndSnapshotAcrossThePublicInterface) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  auto result = RunSource(*context, host, kRegister);
  ASSERT_TRUE(result.success) << result.error.Utf8();
  EXPECT_FALSE(host.last_once);
  result = context->InvokeCallback(host.last, host.Event(true), host);
  EXPECT_TRUE(result.success) << result.error.Utf8();
  EXPECT_EQ("1 True\n", result.stdout_text.Utf8());
  EXPECT_EQ("value-1", host.value);
  result = context->InvokeCallback(host.last, host.Event(), host);
  EXPECT_TRUE(result.success) << result.error.Utf8();
  EXPECT_EQ("2 False\n", result.stdout_text.Utf8());
  result = RunSource(
      *context, host,
      "dom.remove_event_listener(listener)\ndom.remove_event_listener("
      "listener)");
  EXPECT_TRUE(result.success) << result.error.Utf8();
  EXPECT_EQ(2, host.removes);
  EXPECT_TRUE(host.installed.empty());
  EXPECT_FALSE(context->InvokeCallback(host.last, host.Event(), host).success);
}

TEST(PythonDocumentContextTest, ForwardsOnceConsumptionToRuntime) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  std::string source(kRegister);
  source.replace(source.find("once=False"), 10, "once=True");
  ASSERT_TRUE(RunSource(*context, host, source).success);
  EXPECT_TRUE(host.last_once);
  auto result = context->InvokeCallback(host.last, host.Event(true), host);
  EXPECT_TRUE(result.success) << result.error.Utf8();
  EXPECT_EQ("1 True\n", result.stdout_text.Utf8());
  EXPECT_FALSE(context->InvokeCallback(host.last, host.Event(), host).success);
}

TEST(PythonDocumentContextTest,
     CallbackHandlesAreIsolatedAndNativeRemovalIsIdempotent) {
  auto first = CreatePythonDocumentContext();
  auto second = CreatePythonDocumentContext();
  TestHost one, two;
  ASSERT_TRUE(RunSource(*first, one, kRegister).success);
  ASSERT_TRUE(RunSource(*second, two, kRegister).success);
  EXPECT_EQ(one.last.id, two.last.id);
  EXPECT_NE(one.last.context, two.last.context);
  auto result = second->InvokeCallback(one.last, two.Event(), two);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.stopped);
  EXPECT_NE(std::string::npos, result.error.Utf8().find("ReferenceError"));
  EXPECT_FALSE(second->RemoveCallback(one.last).success);
  EXPECT_TRUE(second->InvokeCallback(two.last, two.Event(), two).success);
  EXPECT_TRUE(first->RemoveCallback(one.last).success);
  EXPECT_TRUE(first->RemoveCallback(one.last).success);
  EXPECT_FALSE(first->InvokeCallback(one.last, one.Event(), one).success);
}

TEST(PythonDocumentContextTest, ConvertsHostErrorsAndRollsBackInstallations) {
  for (auto failure :
       {WebPythonDomError::kReference, WebPythonDomError::kNotAllowed,
        WebPythonDomError::kLimit, WebPythonDomError::kState,
        WebPythonDomError::kStopped}) {
    auto context = CreatePythonDocumentContext();
    TestHost host;
    host.failure = failure;
    auto result = RunSource(*context, host, kRegister);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(failure == WebPythonDomError::kStopped, result.stopped);
    if (!result.stopped) {
      const char* exception =
          failure == WebPythonDomError::kReference    ? "ReferenceError"
          : failure == WebPythonDomError::kNotAllowed ? "PermissionError"
          : failure == WebPythonDomError::kLimit      ? "ValueError"
                                                      : "RuntimeError";
      EXPECT_NE(std::string::npos, result.error.Utf8().find(exception));
      EXPECT_NE(std::string::npos,
                result.error.Utf8().find("host listener failure"));
    }
    EXPECT_EQ(1, host.revokes);
    EXPECT_TRUE(host.installed.empty());
    const auto failed = host.last;
    EXPECT_FALSE(context->InvokeCallback(failed, host.Event(), host).success);
    if (!result.stopped) {
      host.failure = WebPythonDomError::kNone;
      result = RunSource(*context, host, kRegister);
      EXPECT_TRUE(result.success) << result.error.Utf8();
      EXPECT_GT(host.last.id, failed.id);
    }
  }
}

TEST(PythonDocumentContextTest,
     RejectsNestedInvocationAndRevokesOnHostInactivation) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  host.on_add = [&] {
    const auto nested = context->InvokeCallback(host.last, host.Event(), host);
    EXPECT_FALSE(nested.success);
    EXPECT_NE(std::string::npos, nested.error.Utf8().find("Nested"));
    host.active = false;
  };
  const auto result = RunSource(*context, host, kRegister);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.stopped);
  EXPECT_EQ(1, host.revokes);
  EXPECT_TRUE(host.installed.empty());
  host.active = true;
  EXPECT_TRUE(context->InvokeCallback(host.last, host.Event(), host).stopped);
}

TEST(PythonDocumentContextTest,
     CallbackCanRemoveItselfAndOrdinaryExceptionsRecover) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  auto result = RunSource(*context, host, R"PY(
import dom
count = 0
def callback(event):
    global count
    count += 1
    if count == 1:
        raise ValueError('first click')
    dom.remove_event_listener(listener)
    print('removed')
listener = dom.add_event_listener(dom.get_element_by_id('button'), 'click', callback)
)PY");
  ASSERT_TRUE(result.success) << result.error.Utf8();
  result = context->InvokeCallback(host.last, host.Event(), host);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.stopped);
  EXPECT_NE(std::string::npos, result.error.Utf8().find("adapter.py"));
  result = context->InvokeCallback(host.last, host.Event(), host);
  EXPECT_TRUE(result.success) << result.error.Utf8();
  EXPECT_EQ("removed\n", result.stdout_text.Utf8());
  EXPECT_TRUE(host.installed.empty());
  EXPECT_FALSE(context->InvokeCallback(host.last, host.Event(), host).success);
}

TEST(PythonDocumentContextTest,
     ReporterCanStopTheSourceBatchBeforeTheNextBlock) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  unsigned reports = 0;
  const auto results = context->Run(
      {{WebString::FromUtf8("value = 42"), WebString::FromUtf8("first")},
       {WebString::FromUtf8("raise AssertionError('must not run')"),
        WebString::FromUtf8("second")}},
      host, [&](const WebPythonScriptResult& result) {
        EXPECT_TRUE(result.success);
        ++reports;
        return false;
      });
  EXPECT_EQ(1u, reports);
  EXPECT_EQ(1u, results.size());
  EXPECT_TRUE(RunSource(*context, host, "assert value == 42").success);
}
}  // namespace
