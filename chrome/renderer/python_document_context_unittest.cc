// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/python_document_context.h"

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
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
  blink::WebPythonBrowserResult Browser(
      const blink::WebPythonBrowserRequest& r) override {
    browser_requests.push_back(r);
    return {{}, "中文", 77, true};
  }
  std::vector<blink::WebPythonBrowserRequest> browser_requests;
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
  blink::WebPythonControlStateResult GetControlState(
      WebPythonDomHandle h) override {
    CheckTarget(h);
    return {{{}, control_failure, "control failure"},
            WebString::FromUtf8(std::string("a\0\xe4\xb8\xad", 5)),
            true,
            false,
            true,
            true};
  }
  WebPythonDomResult UpdateElement(
      WebPythonDomHandle h,
      const blink::WebPythonElementUpdate& update) override {
    CheckTarget(h);
    element_updates.push_back(update);
    return {{}, control_failure, "control failure"};
  }
  std::vector<blink::WebPythonElementUpdate> element_updates;
  WebPythonDomResult AddEventListener(WebPythonDomHandle element,
                                      const WebString& type,
                                      WebPythonCallbackHandle callback,
                                      bool once,
                                      bool passive,
                                      bool capture) override {
    ++adds;
    CheckTarget(element);
    last_type = type.Utf8();
    last_passive = passive;
    last_capture = capture;
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
  WebPythonDomResult RequestFrame(WebPythonCallbackHandle callback) override {
    EXPECT_FALSE(frame.id);
    frame = last_frame = callback;
    return {{}, failure, WebString::FromUtf8("host frame failure")};
  }
  WebPythonDomResult CancelFrame(WebPythonCallbackHandle callback) override {
    ++frame_cancels;
    RevokeFrame(callback);
    return {};
  }
  void RevokeFrame(WebPythonCallbackHandle callback) override {
    if (frame.context == callback.context && frame.id == callback.id) {
      frame = {};
    }
  }
  WebPythonDomResult GetEventTarget(blink::WebPythonTargetKind kind) override {
    return {{target.document, target.generation, 1, kind}};
  }
  blink::WebPythonPageStateResult GetPageState() override {
    return {{}, {true, false, 901, 602, true, 51}};
  }
  WebPythonDomResult ControlEvent(uint64_t token,
                                  blink::WebPythonEventAction action) override {
    last_token = token;
    last_action = action;
    return {{}, control_failure, "control error"};
  }
  WebPythonDomResult SetPointerCapture(WebPythonDomHandle h,
                                       int32_t id,
                                       bool capture) override {
    CheckTarget(h);
    last_pointer = id;
    last_capture = capture;
    return {};
  }
  blink::WebPythonCanvasResult GetCanvasContext(WebPythonDomHandle h) override {
    CheckTarget(h);
    return {{}, canvas_handle};
  }
  blink::WebPythonCanvasResult CreateCanvasSurface(uint32_t w,
                                                   uint32_t h) override {
    canvas_size = {w, h};
    return {{}, surface_handle};
  }
  blink::WebPythonCanvasResult ResizeCanvas(blink::WebPythonCanvasHandle h,
                                            uint32_t w,
                                            uint32_t height) override {
    canvas_target = h;
    canvas_size = {w, height};
    ++h.version;
    return {{}, h};
  }
  WebPythonDomResult ReleaseCanvas(blink::WebPythonCanvasHandle h) override {
    canvas_released = h;
    return {};
  }
  blink::WebPythonCanvasResult SubmitCanvas(
      blink::WebPythonCanvasHandle h,
      const std::vector<blink::WebPythonCanvasCommand>& commands) override {
    canvas_target = h;
    canvas_commands = commands;
    return {{{}, canvas_failure, WebString::FromUtf8("Canvas host failure")},
            h,
            {commands.size(), 456, 789, 0.25}};
  }
  blink::WebPythonCanvasHandle canvas_handle{71, 9, 5, 8};
  blink::WebPythonCanvasHandle surface_handle{71, 9, 7, 11};
  blink::WebPythonCanvasHandle canvas_target;
  blink::WebPythonCanvasHandle canvas_released;
  std::pair<uint32_t, uint32_t> canvas_size;
  std::vector<blink::WebPythonCanvasCommand> canvas_commands;
  WebPythonDomError canvas_failure = WebPythonDomError::kNone;
  std::string last_type;
  bool last_passive = false;
  bool last_capture = false;
  int32_t last_pointer = 0;
  uint64_t last_token = 0;
  blink::WebPythonEventAction last_action =
      blink::WebPythonEventAction::kPreventDefault;
  WebPythonDomError control_failure = WebPythonDomError::kNone;
  WebPythonCallbackHandle TakeFrame() { return std::exchange(frame, {}); }
  WebPythonCallbackEvent Event(bool trusted = false) const {
    return {WebString::FromUtf8("click"), target, trusted};
  }
  void CheckTarget(WebPythonDomHandle element) {
    EXPECT_EQ(target.document, element.document);
    EXPECT_EQ(target.generation, element.generation);
    EXPECT_EQ(
        element.kind == blink::WebPythonTargetKind::kElement ? target.id : 1,
        element.id);
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
  WebPythonCallbackHandle frame;
  WebPythonCallbackHandle last_frame;
  int frame_cancels = 0;
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

TEST(PythonDocumentContextTest, BrowserPreservesTypedPayloadsAndResults) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  auto result = RunSource(*context, host, R"PY(
import browser, dom
browser.storage_set('成绩', '5170')
assert browser.storage_get('成绩') == '中文'
browser.play_tones([('triangle', 220, 620, .15, .07, .09)])
assert browser.request_fullscreen(dom.get_element_by_id('stage')) == 77
assert browser.poll() == (77, True, '中文')
)PY");
  ASSERT_TRUE(result.success) << result.error.Utf8();
  ASSERT_EQ(5u, host.browser_requests.size());
  EXPECT_EQ(blink::WebPythonBrowserOp::kStorageSet,
            host.browser_requests[0].op);
  EXPECT_EQ("成绩", host.browser_requests[0].key.Utf8());
  EXPECT_EQ("5170", host.browser_requests[0].value.Utf8());
  ASSERT_EQ(1u, host.browser_requests[2].tones.size());
  EXPECT_EQ("triangle", host.browser_requests[2].tones[0].wave.Utf8());
  EXPECT_EQ(.07, host.browser_requests[2].tones[0].delay);
  EXPECT_EQ(host.target.document, host.browser_requests[3].element.document);
}

TEST(PythonDocumentContextTest,
     ControlsPreserveUnicodeNulFlagsAndTypedPayload) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  auto result = RunSource(*context, host, R"PY(
import dom
h=dom.get_element_by_id('c')
s=dom.get_control_state(h)
assert s == dict(value='a\0中', checked=True, hidden=False, disabled=True, open=True)
dom.set_attribute(h, 'aria-label', '中文')
dom.toggle_class(h, 'active', True)
dom.set_progress(h, 37.5)
dom.show_modal(h)
dom.close_dialog(h)
)PY");
  ASSERT_TRUE(result.success) << result.error.Utf8();
  ASSERT_EQ(5u, host.element_updates.size());
  EXPECT_EQ("aria-label", host.element_updates[0].name.Utf8());
  EXPECT_EQ("中文", host.element_updates[0].value.Utf8());
  EXPECT_EQ(blink::WebPythonElementAction::kClass,
            host.element_updates[1].action);
  EXPECT_TRUE(host.element_updates[1].enabled);
  EXPECT_EQ(37.5, host.element_updates[2].number);
  EXPECT_EQ(blink::WebPythonElementAction::kCloseDialog,
            host.element_updates[4].action);
}

TEST(PythonDocumentContextTest,
     ControlsMapNativeDenialsWithoutExecutingScript) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  host.control_failure = WebPythonDomError::kNotAllowed;
  auto result = RunSource(*context, host, R"PY(
import dom
h=dom.get_element_by_id('c')
for f, args in ((dom.get_control_state,(h,)), (dom.set_value,(h,'x'))):
    try: f(*args)
    except PermissionError: pass
    else: raise AssertionError('native denial lost')
)PY");
  EXPECT_TRUE(result.success) << result.error.Utf8();
}

TEST(PythonDocumentContextTest,
     CanvasPreservesTypedPayloadsVersionsAndMetrics) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  auto result = RunSource(*context, host, R"PY(
import canvas, dom
c = canvas.get_context_2d(dom.get_element_by_id('game'))
s = canvas.create_surface(16, 24)
b = canvas.batch()
g = b.linear_gradient(0, 1, 2, 3)
g.add_color_stop(0.5, '#abcdef80')
b.fill_style(g)
b.stroke_style('#123')
b.line_dash([2, 3.5])
b.fill_text('画布', -1.5, 25)
b.draw_surface(s, 4, 5, 32, 48)
r = canvas.submit(c, b)
assert r == dict(commands=7, bytes=456, pixels=789, native_ms=0.25)
c = canvas.resize(c, 640, 360)
canvas.submit(c, b)
canvas.release(s)
)PY");
  ASSERT_TRUE(result.success) << result.error.Utf8();
  EXPECT_EQ(640u, host.canvas_size.first);
  EXPECT_EQ(360u, host.canvas_size.second);
  EXPECT_EQ(71u, host.canvas_target.document);
  EXPECT_EQ(9u, host.canvas_target.generation);
  EXPECT_EQ(5u, host.canvas_target.id);
  EXPECT_EQ(9u, host.canvas_target.version);
  ASSERT_EQ(7u, host.canvas_commands.size());
  const auto& gradient = host.canvas_commands[0];
  EXPECT_EQ(blink::WebPythonCanvasOp::kLinearGradient, gradient.op);
  EXPECT_EQ(1u, gradient.index);
  EXPECT_EQ(3, gradient.args[3]);
  EXPECT_EQ("#abcdef80", host.canvas_commands[1].text);
  EXPECT_EQ(1u, host.canvas_commands[2].index);
  EXPECT_EQ((std::vector<double>{2, 3.5}), host.canvas_commands[4].values);
  EXPECT_EQ("画布", host.canvas_commands[5].text);
  EXPECT_EQ(-1.5, host.canvas_commands[5].args[0]);
  EXPECT_EQ(blink::WebPythonCanvasOp::kDrawSurface, host.canvas_commands[6].op);
  EXPECT_EQ(7u, host.canvas_commands[6].source.id);
  EXPECT_EQ(11u, host.canvas_commands[6].source.version);
  EXPECT_EQ(7u, host.canvas_released.id);
  EXPECT_EQ(11u, host.canvas_released.version);
}

TEST(PythonDocumentContextTest, CanvasMapsRecoverableNativeErrors) {
  for (auto error :
       {WebPythonDomError::kReference, WebPythonDomError::kLimit,
        WebPythonDomError::kState, WebPythonDomError::kNotAllowed}) {
    auto context = CreatePythonDocumentContext();
    TestHost host;
    host.canvas_failure = error;
    auto result = RunSource(*context, host, R"PY(
import canvas
c = canvas.create_surface(1, 1)
b = canvas.batch()
try: canvas.submit(c, b)
except (ReferenceError, ValueError, RuntimeError, PermissionError) as e:
    assert str(e) == 'Canvas host failure'
else: raise AssertionError('native error was dropped')
)PY");
    ASSERT_TRUE(result.success) << result.error.Utf8();
    host.canvas_failure = WebPythonDomError::kNone;
    EXPECT_TRUE(RunSource(*context, host, "canvas.submit(c,b)").success);
  }
}

TEST(PythonDocumentContextTest, CanvasDrawingStopCannotBeCaughtAndResumed) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  host.canvas_failure = WebPythonDomError::kDrawingLimit;
  auto result = RunSource(*context, host, R"PY(
import canvas
c = canvas.create_surface(1, 1)
try: canvas.submit(c, canvas.batch())
except RuntimeError: pass
)PY");
  EXPECT_FALSE(result.success);
  host.canvas_failure = WebPythonDomError::kNone;
  EXPECT_FALSE(RunSource(*context, host, "pass").success);
}

TEST(PythonDocumentContextTest, InputTargetsPageStateAndCaptureKeepTheirTypes) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  auto result = RunSource(*context, host, R"PY(
import dom
page = dom.get_page_state()
assert (page.hidden, page.has_focus, page.width, page.height,
        page.reduced_motion, page.input_epoch) == (True, False, 901, 602, True, 51)
for target, cls, event_type in ((dom.get_document(), dom.DocumentHandle, 'keyup'),
                                 (dom.get_window(), dom.WindowHandle, 'resize')):
    assert type(target) is cls
    listener = dom.add_event_listener(target, event_type, lambda e: None,
                                     once=True, passive=True, capture=True)
    dom.remove_event_listener(listener)
element = dom.get_element_by_id('button')
dom.set_pointer_capture(element, -25)
dom.release_pointer_capture(element, -25)
)PY");
  EXPECT_TRUE(result.success) << result.error.Utf8();
  EXPECT_EQ("resize", host.last_type);
  EXPECT_TRUE(host.last_once && host.last_passive);
  EXPECT_EQ(-25, host.last_pointer);
  EXPECT_FALSE(host.last_capture);
  EXPECT_EQ(2, host.removes);
}

TEST(PythonDocumentContextTest, ForwardsFullInputSnapshotAndEventControls) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  auto result = RunSource(*context, host, R"PY(
import dom
def callback(e):
    assert type(e.current_target) is dom.DocumentHandle
    assert (e.type, e.is_trusted, e.cancelable, e.bubbles, e.default_prevented,
            e.passive, e.target_is_editable, e.target_in_dialog) == ('keyup', True, True, True, True, True, True, True)
    assert (e.key, e.code, e.repeat, e.is_composing, e.alt_key, e.ctrl_key,
            e.meta_key, e.shift_key) == ('界', 'Space', True, True, True, True, True, True)
    assert (e.pointer_id, e.pointer_type, e.button, e.buttons, e.client_x,
            e.client_y, e.is_primary, e.value, e.checked, e.value_truncated,
            e.input_epoch) == (-99, 'pen', -1, 31, 1.5, -2.5, True, '值', True, True, 72)
    e.prevent_default()
    e.stop_propagation()
    e.stop_immediate_propagation()
listener = dom.add_event_listener(dom.get_document(), 'keyup', callback)
)PY");
  ASSERT_TRUE(result.success) << result.error.Utf8();
  WebPythonCallbackEvent e;
  e.type = WebString::FromUtf8("keyup");
  e.current_target = {host.target.document, host.target.generation, 1,
                      blink::WebPythonTargetKind::kDocument};
  e.is_trusted = e.cancelable = e.bubbles = e.default_prevented = e.passive =
      true;
  e.target_is_editable = e.target_in_dialog = true;
  e.key = WebString::FromUtf8("界");
  e.code = WebString::FromUtf8("Space");
  e.repeat = e.is_composing = e.alt_key = e.ctrl_key = e.meta_key =
      e.shift_key = true;
  e.pointer_id = -99;
  e.pointer_type = WebString::FromUtf8("pen");
  e.button = -1;
  e.buttons = 31;
  e.client_x = 1.5;
  e.client_y = -2.5;
  e.is_primary = true;
  e.value = WebString::FromUtf8("值");
  e.checked = e.value_truncated = true;
  e.input_epoch = 72;
  e.control_token = 1234;
  result = context->InvokeCallback(host.last, e, host);
  EXPECT_TRUE(result.success) << result.error.Utf8();
  EXPECT_EQ(1234u, host.last_token);
  EXPECT_EQ(blink::WebPythonEventAction::kStopImmediatePropagation,
            host.last_action);
  host.control_failure = WebPythonDomError::kNotAllowed;
  result = context->InvokeCallback(host.last, e, host);
  EXPECT_FALSE(result.success);
  EXPECT_NE(std::string::npos, result.error.Utf8().find("PermissionError"));
}

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

TEST(PythonDocumentContextTest, CallbackWindowRefusalIsNonfatalAndConverted) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  ASSERT_TRUE(RunSource(*context, host, R"PY(
import dom
def callback(event): print('x' * 3072, end='')
dom.add_event_listener(dom.get_element_by_id('button'), 'click', callback)
)PY")
                  .success);
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(context->CanInvokeCallback());
    ASSERT_TRUE(context->InvokeCallback(host.last, host.Event(), host).success);
  }
  EXPECT_FALSE(context->CanInvokeCallback());
  const auto skipped = context->InvokeCallback(host.last, host.Event(), host);
  EXPECT_TRUE(skipped.callback_skipped);
  EXPECT_FALSE(skipped.stopped);
  EXPECT_TRUE(skipped.error.IsEmpty());
  EXPECT_TRUE(skipped.stdout_text.IsEmpty());
  EXPECT_TRUE(
      RunSource(*context, host, "print('initialization budget is separate')")
          .success);
}

TEST(PythonDocumentContextTest,
     ForwardsAnimationTimestampAndOneShotConsumption) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  ASSERT_TRUE(RunSource(*context, host, R"PY(
import animation, dom
target = dom.get_element_by_id('result')
def frame(timestamp):
    assert type(timestamp) is float
    dom.set_text(target, str(timestamp))
    animation.request_frame(frame)
request = animation.request_frame(frame)
)PY")
                  .success);
  EXPECT_TRUE(context->CanInvokeFrameCallback());
  const auto first = host.TakeFrame();
  auto result = context->InvokeFrameCallback(first, 123.5, host);
  EXPECT_TRUE(result.success) << result.error.Utf8();
  EXPECT_EQ("123.5", host.value);
  EXPECT_GT(host.frame.id, first.id);
  EXPECT_FALSE(context->InvokeFrameCallback(first, 456, host).success);
  result = RunSource(*context, host, "animation.cancel_frame(request)");
  EXPECT_TRUE(result.success) << result.error.Utf8();
  EXPECT_NE(0u, host.frame.id);
  EXPECT_EQ(1, host.frame_cancels);
  result = context->InvokeFrameCallback(host.TakeFrame(), 456, host);
  EXPECT_TRUE(result.success) << result.error.Utf8();
  EXPECT_EQ("456.0", host.value);
}

TEST(PythonDocumentContextTest, AnimationCancelAndErrorsUsePublicHostContract) {
  auto context = CreatePythonDocumentContext();
  TestHost host;
  auto result = RunSource(*context, host, R"PY(
import animation
request = animation.request_frame(lambda timestamp: None)
animation.cancel_frame(request)
animation.cancel_frame(request)
)PY");
  ASSERT_TRUE(result.success) << result.error.Utf8();
  EXPECT_FALSE(host.frame.id);
  EXPECT_EQ(2, host.frame_cancels);
  EXPECT_FALSE(context->InvokeFrameCallback(host.last_frame, 1, host).success);
  for (auto failure :
       {WebPythonDomError::kReference, WebPythonDomError::kNotAllowed,
        WebPythonDomError::kLimit, WebPythonDomError::kState}) {
    host.failure = failure;
    result = RunSource(*context, host,
                       "animation.request_frame(lambda timestamp: None)");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.stopped);
    EXPECT_NE(std::string::npos,
              result.error.Utf8().find("host frame failure"));
    EXPECT_FALSE(host.frame.id);
  }
}
}  // namespace
