// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/html_py_script_element.h"

#include <optional>

#include "base/check.h"
#include "base/memory/raw_ref.h"
#include "base/test/bind.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/web/web_navigation_params.h"
#include "third_party/blink/public/web/web_python_script_runner.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_keyboard_event_init.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_pointer_event_init.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/core/dom/scripted_animation_controller.h"
#include "third_party/blink/renderer/core/event_type_names.h"
#include "third_party/blink/renderer/core/events/keyboard_event.h"
#include "third_party/blink/renderer/core/events/pointer_event.h"
#include "third_party/blink/renderer/core/frame/csp/content_security_policy.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/html/html_head_element.h"
#include "third_party/blink/renderer/core/html/html_unknown_element.h"
#include "third_party/blink/renderer/core/html/python_script_controller.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/testing/mock_policy_container_host.h"
#include "third_party/blink/renderer/core/testing/sim/sim_request.h"
#include "third_party/blink/renderer/core/testing/sim/sim_test.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/bindings/script_forbidden_scope.h"
#include "third_party/blink/renderer/platform/heap/thread_state.h"
#include "third_party/blink/renderer/platform/scheduler/public/frame_scheduler.h"
#include "third_party/blink/renderer/platform/testing/runtime_enabled_features_test_helpers.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"
#include "url/url_util.h"
#include "v8/include/cppgc/heap-state.h"

namespace blink {
namespace {

class RecordingPythonRunner final : public WebPythonScriptRunner {
 public:
  class Context final : public WebPythonScriptContext {
   public:
    explicit Context(RecordingPythonRunner& runner)
        : runner_(runner), destructions_(runner.destructions) {}
    ~Context() override {
      CHECK(!ScriptForbiddenScope::IsScriptForbidden());
      CHECK(!ThreadState::Current()->IsSweepingOnOwningThread());
      CHECK(!cppgc::subtle::HeapState::IsInAtomicPause(
          ThreadState::Current()->heap_handle()));
      ++*destructions_;
    }
    std::vector<WebPythonScriptResult> Run(
        const std::vector<WebPythonScriptSource>& sources,
        WebPythonDomHost& dom_host,
        ReportResult report_result) override {
      runner_->host = &dom_host;
      auto results = runner_->Run(sources);
      for (size_t i = 0; i < results.size(); ++i) {
        const bool keep_going = report_result(results[i]);
        if (!keep_going || results[i].stopped) {
          results.resize(i + 1);
          break;
        }
      }
      runner_->host = nullptr;
      return results;
    }

    WebPythonScriptResult InvokeCallback(WebPythonCallbackHandle callback,
                                         const WebPythonCallbackEvent& event,
                                         WebPythonDomHost& dom_host) override {
      runner_->invoked.push_back(callback.id);
      runner_->last_event = event;
      runner_->host = &dom_host;
      if (runner_->on_click) {
        runner_->on_click.Run();
      }
      runner_->host = nullptr;
      return runner_->callback_result;
    }
    bool CanInvokeCallback() override { return runner_->callback_admitted; }
    bool CanInvokeFrameCallback() override { return runner_->frame_admitted; }
    WebPythonScriptResult InvokeFrameCallback(
        WebPythonCallbackHandle callback,
        double timestamp_ms,
        WebPythonDomHost& dom_host) override {
      runner_->frames.push_back(callback.id);
      runner_->timestamps.push_back(timestamp_ms);
      runner_->host = &dom_host;
      if (runner_->on_frame) {
        runner_->on_frame.Run();
      }
      runner_->host = nullptr;
      return runner_->callback_result;
    }
    WebPythonScriptResult RemoveCallback(WebPythonCallbackHandle) override {
      ADD_FAILURE() << "Unexpected callback removal in parser-only tests";
      return {};
    }

   private:
    const raw_ref<RecordingPythonRunner> runner_;
    std::shared_ptr<unsigned> destructions_;
  };
  std::unique_ptr<WebPythonScriptContext> CreateContext() override {
    ++contexts_created;
    return std::make_unique<Context>(*this);
  }
  bool IsOriginAllowed(const WebSecurityOrigin& origin) const override {
    return allow && origin.ToString() == "https://example.test";
  }
  bool AllowsUnrestrictedPages() const override { return unrestricted; }
  std::vector<WebPythonScriptResult> Run(
      const std::vector<WebPythonScriptSource>& sources) {
    ++batches;
    executed = sources;
    if (on_run) {
      on_run.Run();
    }
    if (supplied_results) {
      return *supplied_results;
    }
    std::vector<WebPythonScriptResult> results(sources.size());
    for (auto& result : results) {
      result.success = true;
    }
    return results;
  }
  bool allow = true;
  bool unrestricted = false;
  bool callback_admitted = true;
  bool frame_admitted = true;
  unsigned contexts_created = 0;
  std::shared_ptr<unsigned> destructions = std::make_shared<unsigned>(0);
  raw_ptr<WebPythonDomHost> host = nullptr;
  unsigned batches = 0;
  base::RepeatingClosure on_run;
  base::RepeatingClosure on_click;
  base::RepeatingClosure on_frame;
  std::vector<uint64_t> frames;
  std::vector<double> timestamps;
  std::vector<uint64_t> invoked;
  WebPythonCallbackEvent last_event;
  WebPythonScriptResult callback_result{true};
  std::vector<WebPythonScriptSource> executed;
  std::optional<std::vector<WebPythonScriptResult>> supplied_results;
};

class HTMLPyScriptElementTest : public SimTest {
 protected:
  void SetUp() override {
    SimTest::SetUp();
    WebPythonScriptRunner::SetForProcess(&runner_);
  }
  void TearDown() override {
    WebPythonScriptRunner::SetForProcess(nullptr);
    SimTest::TearDown();
  }
  void Load(const String& html, SimRequest::Params params = {}) {
    SimRequest main("https://example.test/", "text/html", params);
    LoadURL("https://example.test/");
    main.Complete(html);
    test::RunPendingTasks();
  }
  void Click(const char* id = "button", bool trusted = false) {
    auto* event = Event::CreateBubble(event_type_names::kClick);
    if (trusted) {
      GetElementById(id)->DispatchEvent(*event);
    } else {
      GetElementById(id)->dispatchEventForBindings(event, ASSERT_NO_EXCEPTION);
    }
  }
  void RegisterClick(bool once = false) {
    runner_.on_run = base::BindLambdaForTesting([&, once] {
      auto& host = *runner_.host;
      auto target = host.GetElementById("button").handle;
      EXPECT_EQ(WebPythonDomError::kNone,
                host.AddEventListener(target, "click", {1, 1}, once).error);
    });
    Load(
        "<body><button id=button><span id=child>click</span></button>"
        "<p id=result>before</p><pyscript id=python>register</pyscript>");
  }
  void RegisterFrame() {
    runner_.on_run = base::BindLambdaForTesting([&] {
      EXPECT_EQ(WebPythonDomError::kNone,
                runner_.host->RequestFrame({1, 1}).error);
    });
    Load("<body><p id=result>before</p><pyscript id=python>frame</pyscript>");
  }
  void Tick(double timestamp) {
    auto& animation = GetDocument().GetScriptedAnimationController();
    animation.SetCurrentFrameTimeMs(timestamp);
    animation.ExecuteFrameCallbacks();
  }
  ScopedPythonScriptForTest enabled_{true};
  RecordingPythonRunner runner_;
};

TEST_F(HTMLPyScriptElementTest,
       DetachedListenerCanBeRemovedWithoutReattaching) {
  WebPythonDomHandle handle;
  runner_.on_run = base::BindLambdaForTesting([&] {
    handle = runner_.host->GetElementById("button").handle;
    EXPECT_EQ(
        WebPythonDomError::kNone,
        runner_.host->AddEventListener(handle, "click", {1, 1}, false).error);
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host->RequestFrame({1, 2}).error);
    auto forbidden = runner_.host->GetElementById("custom").handle;
    EXPECT_EQ(WebPythonDomError::kNotAllowed,
              runner_.host->AddEventListener(forbidden, "click", {1, 3}, false)
                  .error);
  });
  Load(
      "<body><button id=button>click</button><x-native "
      "id=custom></x-native><pyscript>cancel</pyscript>");
  Persistent<Element> original(GetElementById("button"));
  original->remove();
  runner_.on_frame = base::BindLambdaForTesting([&] {
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host->RemoveEventListener(handle, {1, 1}).error);
  });
  Tick(10);
  GetDocument().body()->AppendChild(original);
  Click();
  EXPECT_TRUE(runner_.invoked.empty());
}

TEST_F(HTMLPyScriptElementTest, PageStateDoesNotForceInitialLayout) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    ASSERT_FALSE(GetDocument().View()->DidFirstLayout());
    auto result = runner_.host->GetPageState();
    EXPECT_EQ(WebPythonDomError::kNone, result.status.error);
    EXPECT_GT(result.state.width, 0);
    EXPECT_GT(result.state.height, 0);
    EXPECT_FALSE(GetDocument().View()->DidFirstLayout());
  });
  Load("<body><p>Unlaid out</p><pyscript>state</pyscript>");
  EXPECT_EQ(1u, runner_.batches);
}

TEST_F(HTMLPyScriptElementTest,
       TypedTargetsRejectElementOperationsAndWrongEvents) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    auto doc = host.GetEventTarget(WebPythonTargetKind::kDocument).handle;
    auto win = host.GetEventTarget(WebPythonTargetKind::kWindow).handle;
    EXPECT_EQ(WebPythonTargetKind::kDocument, doc.kind);
    EXPECT_EQ(WebPythonTargetKind::kWindow, win.kind);
    EXPECT_EQ(WebPythonDomError::kReference, host.SetText(doc, "deny").error);
    EXPECT_EQ(WebPythonDomError::kReference,
              host.SetPointerCapture(win, 1, true).error);
    EXPECT_EQ(WebPythonDomError::kNotAllowed,
              host.AddEventListener(doc, "click", {1, 1}, false).error);
    EXPECT_EQ(WebPythonDomError::kNotAllowed,
              host.AddEventListener(win, "keydown", {1, 2}, false).error);
    auto foreign = doc;
    ++foreign.generation;
    EXPECT_EQ(WebPythonDomError::kReference,
              host.AddEventListener(foreign, "keyup", {1, 3}, false).error);
    EXPECT_EQ(WebPythonDomError::kNone,
              host.AddEventListener(doc, "keyup", {1, 1}, false).error);
    EXPECT_EQ(WebPythonDomError::kNone,
              host.AddEventListener(win, "resize", {1, 2}, false).error);
    EXPECT_EQ(WebPythonDomError::kNone,
              host.RemoveEventListener(doc, {1, 1}).error);
    auto state = host.GetPageState();
    EXPECT_EQ(WebPythonDomError::kNone, state.status.error);
    EXPECT_GT(state.state.width, 0);
    EXPECT_GT(state.state.height, 0);
  });
  Load("<body><pyscript>input</pyscript>");
  GetDocument().DispatchEvent(*Event::Create(event_type_names::kKeyup));
  GetDocument().domWindow()->DispatchEvent(
      *Event::Create(event_type_names::kResize));
  EXPECT_EQ((std::vector<uint64_t>{2}), runner_.invoked);
  EXPECT_EQ(WebPythonTargetKind::kWindow,
            runner_.last_event.current_target.kind);
}

TEST_F(HTMLPyScriptElementTest,
       KeyboardSnapshotIncludesEditableAndDialogMetadata) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto doc =
        runner_.host->GetEventTarget(WebPythonTargetKind::kDocument).handle;
    EXPECT_EQ(
        WebPythonDomError::kNone,
        runner_.host->AddEventListener(doc, "keydown", {1, 1}, false).error);
  });
  Load(
      "<body><dialog open><input id=button></dialog><pyscript>keys</pyscript>");
  auto* init = KeyboardEventInit::Create();
  init->setBubbles(true);
  init->setCancelable(true);
  init->setKey(" ");
  init->setCode("Space");
  init->setRepeat(true);
  init->setIsComposing(true);
  init->setCtrlKey(true);
  init->setShiftKey(true);
  auto* script_state = ToScriptStateForMainWorld(GetDocument().GetFrame());
  ScriptState::Scope scope(script_state);
  auto* event =
      KeyboardEvent::Create(script_state, event_type_names::kKeydown, init);
  GetElementById("button")->dispatchEventForBindings(event,
                                                     ASSERT_NO_EXCEPTION);
  ASSERT_EQ(1u, runner_.invoked.size());
  auto& e = runner_.last_event;
  EXPECT_EQ("Space", e.code);
  EXPECT_EQ(" ", e.key);
  EXPECT_TRUE(e.repeat && e.is_composing && e.ctrl_key && e.shift_key);
  EXPECT_TRUE(e.cancelable && e.bubbles);
  EXPECT_TRUE(e.target_is_editable && e.target_in_dialog);
  EXPECT_FALSE(e.is_trusted);
  EXPECT_EQ(WebPythonTargetKind::kDocument, e.current_target.kind);
}

TEST_F(HTMLPyScriptElementTest,
       EventControlsPreserveCaptureAndImmediatePropagation) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    auto target = host.GetElementById("button").handle;
    auto doc = host.GetEventTarget(WebPythonTargetKind::kDocument).handle;
    EXPECT_EQ(WebPythonDomError::kNone,
              host.AddEventListener(doc, "keydown", {1, 1}, false, false, true)
                  .error);
    EXPECT_EQ(WebPythonDomError::kNone,
              host.AddEventListener(target, "click", {1, 2}, false).error);
    EXPECT_EQ(WebPythonDomError::kNone,
              host.AddEventListener(target, "click", {1, 3}, false).error);
  });
  Load("<body><button id=button>click</button><pyscript>control</pyscript>");
  runner_.on_click = base::BindLambdaForTesting([&] {
    auto token = runner_.last_event.control_token;
    EXPECT_NE(0u, token);
    EXPECT_EQ(
        WebPythonDomError::kNone,
        runner_.host->ControlEvent(token, WebPythonEventAction::kPreventDefault)
            .error);
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host
                  ->ControlEvent(
                      token, WebPythonEventAction::kStopImmediatePropagation)
                  .error);
  });
  auto* event = Event::CreateCancelableBubble(event_type_names::kClick);
  GetElementById("button")->DispatchEvent(*event);
  EXPECT_TRUE(event->defaultPrevented());
  // Dispatch clears transient propagation flags; the second listener must not
  // run.
  EXPECT_EQ((std::vector<uint64_t>{2}), runner_.invoked);
}

TEST_F(HTMLPyScriptElementTest,
       PassiveCannotPreventDefaultAndNonCancelableIsNoop) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto h = runner_.host->GetElementById("button").handle;
    EXPECT_EQ(
        WebPythonDomError::kNone,
        runner_.host->AddEventListener(h, "click", {1, 1}, false, true).error);
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host->AddEventListener(h, "change", {1, 2}, false).error);
  });
  Load("<body><input id=button><pyscript>control</pyscript>");
  runner_.on_click = base::BindLambdaForTesting([&] {
    EXPECT_EQ(runner_.last_event.passive ? WebPythonDomError::kNotAllowed
                                         : WebPythonDomError::kNone,
              runner_.host
                  ->ControlEvent(runner_.last_event.control_token,
                                 WebPythonEventAction::kPreventDefault)
                  .error);
  });
  auto* click = Event::CreateCancelableBubble(event_type_names::kClick);
  GetElementById("button")->DispatchEvent(*click);
  EXPECT_FALSE(click->defaultPrevented());
  auto* change = Event::Create(event_type_names::kChange);
  GetElementById("button")->DispatchEvent(*change);
  EXPECT_FALSE(change->defaultPrevented());
  EXPECT_EQ(2u, runner_.invoked.size());
}

TEST_F(HTMLPyScriptElementTest, EventTokenExpiresBeforeNextDispatchAndFrame) {
  RegisterClick();
  uint64_t saved = 0;
  runner_.on_click = base::BindLambdaForTesting([&] {
    auto current = runner_.last_event.control_token;
    if (saved) {
      EXPECT_EQ(WebPythonDomError::kReference,
                runner_.host
                    ->ControlEvent(saved, WebPythonEventAction::kPreventDefault)
                    .error);
    }
    saved = current;
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host->RequestFrame({1, 8}).error);
  });
  runner_.on_frame = base::BindLambdaForTesting([&] {
    EXPECT_EQ(
        WebPythonDomError::kReference,
        runner_.host->ControlEvent(saved, WebPythonEventAction::kPreventDefault)
            .error);
  });
  Click();
  Tick(10);
  Click();
  Tick(30);
  EXPECT_EQ(2u, runner_.invoked.size());
  EXPECT_EQ(2u, runner_.frames.size());
}

TEST_F(HTMLPyScriptElementTest,
       SkippedKeyupChangesInputEpochWithoutConsumingOnce) {
  uint64_t before = 0;
  runner_.on_run = base::BindLambdaForTesting([&] {
    before = runner_.host->GetPageState().state.input_epoch;
    auto doc =
        runner_.host->GetEventTarget(WebPythonTargetKind::kDocument).handle;
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host->AddEventListener(doc, "keyup", {1, 1}, true).error);
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host->RequestFrame({1, 2}).error);
  });
  Load("<pyscript>keyup</pyscript>");
  runner_.callback_admitted = false;
  GetDocument().DispatchEvent(*Event::Create(event_type_names::kKeyup));
  EXPECT_TRUE(runner_.invoked.empty());
  runner_.on_frame = base::BindLambdaForTesting([&] {
    EXPECT_GT(runner_.host->GetPageState().state.input_epoch, before);
  });
  Tick(10);
  runner_.callback_admitted = true;
  GetDocument().DispatchEvent(*Event::Create(event_type_names::kKeyup));
  GetDocument().DispatchEvent(*Event::Create(event_type_names::kKeyup));
  EXPECT_EQ(1u, runner_.invoked.size());
  EXPECT_GT(runner_.last_event.input_epoch, before);
}

TEST_F(HTMLPyScriptElementTest,
       BlurAndVisibilityResetStateWithoutPythonAdmission) {
  RegisterFrame();
  runner_.callback_admitted = false;
  GetDocument().domWindow()->DispatchEvent(
      *Event::Create(event_type_names::kBlur));
  GetDocument().DispatchEvent(
      *Event::Create(event_type_names::kVisibilitychange));
  runner_.on_frame = base::BindLambdaForTesting(
      [&] { EXPECT_EQ(2u, runner_.host->GetPageState().state.input_epoch); });
  Tick(10);
  EXPECT_EQ(1u, runner_.frames.size());
}

TEST_F(HTMLPyScriptElementTest, FormSnapshotsUseRetargetedValueAndBoundUtf8) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto h = runner_.host->GetElementById("button").handle;
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host->AddEventListener(h, "input", {1, 1}, false).error);
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host->AddEventListener(h, "change", {1, 2}, false).error);
  });
  Load(
      "<body><input id=button type=checkbox checked "
      "value=value><pyscript>form</pyscript>");
  GetElementById("button")->DispatchEvent(
      *Event::CreateBubble(event_type_names::kChange));
  EXPECT_EQ("value", runner_.last_event.value);
  EXPECT_TRUE(runner_.last_event.checked);
  EXPECT_FALSE(runner_.last_event.value_truncated);
  GetElementById("button")->setAttribute(
      html_names::kValueAttr,
      AtomicString(String::FromUtf8(std::string(20000, 'a'))));
  GetElementById("button")->DispatchEvent(
      *Event::CreateBubble(event_type_names::kInput));
  EXPECT_EQ(16384u, runner_.last_event.value.Utf8().size());
  EXPECT_TRUE(runner_.last_event.value_truncated);
}

TEST_F(HTMLPyScriptElementTest,
       PointerSnapshotAndInvalidCaptureDoNotCreateAnActivePointer) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto h = runner_.host->GetElementById("button").handle;
    EXPECT_EQ(
        WebPythonDomError::kNone,
        runner_.host->AddEventListener(h, "pointerdown", {1, 1}, false).error);
  });
  Load("<body><button id=button>pointer</button><pyscript>pointer</pyscript>");
  runner_.on_click = base::BindLambdaForTesting([&] {
    EXPECT_EQ(
        WebPythonDomError::kState,
        runner_.host
            ->SetPointerCapture(runner_.last_event.current_target, 123, true)
            .error);
  });
  auto* init = PointerEventInit::Create();
  init->setPointerId(123);
  init->setPointerType("pen");
  init->setButton(1);
  init->setButtons(4);
  init->setClientX(12.5);
  init->setClientY(24.5);
  init->setIsPrimary(true);
  auto* event = PointerEvent::Create(event_type_names::kPointerdown, init);
  GetElementById("button")->dispatchEventForBindings(event,
                                                     ASSERT_NO_EXCEPTION);
  auto& e = runner_.last_event;
  EXPECT_EQ(123, e.pointer_id);
  EXPECT_EQ("pen", e.pointer_type);
  EXPECT_EQ(1, e.button);
  EXPECT_EQ(4, e.buttons);
  EXPECT_EQ(12.5, e.client_x);
  EXPECT_EQ(24.5, e.client_y);
  EXPECT_TRUE(e.is_primary);
  EXPECT_FALSE(e.is_trusted);
}

TEST_F(HTMLPyScriptElementTest,
       GlobalTargetsAndResetObserversAreRevokedOnNavigation) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto doc =
        runner_.host->GetEventTarget(WebPythonTargetKind::kDocument).handle;
    auto win =
        runner_.host->GetEventTarget(WebPythonTargetKind::kWindow).handle;
    EXPECT_EQ(
        WebPythonDomError::kNone,
        runner_.host->AddEventListener(doc, "keyup", {1, 1}, false).error);
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host->AddEventListener(win, "blur", {1, 2}, false).error);
  });
  Load("<pyscript>global</pyscript>");
  Persistent<Document> old(&GetDocument());
  Persistent<LocalDOMWindow> window(old->domWindow());
  runner_.on_run.Reset();
  Load("<p>new page</p>");
  old->DispatchEvent(*Event::Create(event_type_names::kKeyup));
  window->DispatchEvent(*Event::Create(event_type_names::kBlur));
  EXPECT_TRUE(runner_.invoked.empty());
  EXPECT_EQ(1u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest,
       RunsAfterDeferredScriptsBeforeDOMContentLoaded) {
  SimRequest main("https://example.test/", "text/html");
  SimSubresourceRequest deferred("https://example.test/deferred.js",
                                 "text/javascript");
  runner_.on_run = base::BindLambdaForTesting([&] {
    EXPECT_EQ(Document::kInteractive, GetDocument().GetReadyState());
    EXPECT_EQ("deferred", GetElementById("marker")->textContent());
  });
  LoadURL("https://example.test/");
  main.Complete(R"HTML(
    <p id="marker">parsed</p><pyscript>print(42)</pyscript>
    <script defer src="deferred.js"></script>
    <script>document.addEventListener('DOMContentLoaded', () => {
      document.querySelector('#marker').textContent = 'dcl';
    });</script>
  )HTML");
  test::RunPendingTasks();
  EXPECT_EQ(0u, runner_.batches);
  deferred.Complete(
      "document.querySelector('#marker').textContent='deferred';");
  test::RunPendingTasks();
  ASSERT_EQ(1u, runner_.batches);
  ASSERT_EQ(1u, runner_.executed.size());
  EXPECT_EQ("print(42)", runner_.executed[0].source);
  EXPECT_EQ("dcl", GetElementById("marker")->textContent());
}

TEST_F(HTMLPyScriptElementTest,
       ImmutableSnapshotAndNoDynamicOrTemplateExecution) {
  Load(R"HTML(<body><pyscript id="first">original</pyscript>
    <template><pyscript>template</pyscript></template>
    <script>
      const first = document.querySelector('#first');
      first.textContent = 'changed';
      document.body.append(first.cloneNode(true));
      const dynamic = document.createElement('pyscript');
      dynamic.textContent = 'dynamic'; document.body.append(dynamic);
      const fragment = document.createElement('div');
      fragment.innerHTML = '<pyscript>fragment</pyscript>';
      document.body.append(fragment);
      first.remove(); document.body.append(first);
    </script><pyscript>second</pyscript>)HTML");
  ASSERT_EQ(1u, runner_.batches);
  ASSERT_EQ(2u, runner_.executed.size());
  EXPECT_EQ("original", runner_.executed[0].source);
  EXPECT_EQ("second", runner_.executed[1].source);
}

TEST_F(HTMLPyScriptElementTest, DisabledFeatureKeepsUnknownElementAndMarkup) {
  ScopedPythonScriptForTest disabled(false);
  Load("<pyscript id=python><b id=nested>text</b>&amp;</pyscript>");
  EXPECT_TRUE(IsA<HTMLUnknownElement>(GetElementById("python")));
  EXPECT_TRUE(GetElementById("nested"));
  EXPECT_EQ(0u, runner_.batches);
}

TEST_F(HTMLPyScriptElementTest, RawTextSurvivesChunksAndMixedCaseEndTag) {
  SimRequest main("https://example.test/", "text/html");
  LoadURL("https://example.test/");
  main.Write("<pyscript id=python>\nif 1 < 2:\n    print('&amp; <img src=x>')");
  main.Write("\n</PySc");
  main.Write("RiPt>");
  main.Finish();
  test::RunPendingTasks();
  ASSERT_EQ(1u, runner_.executed.size());
  EXPECT_EQ("\nif 1 < 2:\n    print('&amp; <img src=x>')\n",
            runner_.executed[0].source);
  EXPECT_EQ(nullptr, GetDocument().QuerySelector(AtomicString("img")));
  EXPECT_TRUE(IsA<HTMLPyScriptElement>(GetElementById("python")));
}

TEST_F(HTMLPyScriptElementTest, RejectsOriginAndAbsentEmbedder) {
  runner_.allow = false;
  Load("<pyscript>wrong origin</pyscript>");
  EXPECT_EQ(0u, runner_.batches);
  WebPythonScriptRunner::SetForProcess(nullptr);
  Load("<pyscript>no embedder</pyscript>");
  EXPECT_EQ(0u, runner_.batches);
}

TEST_F(HTMLPyScriptElementTest, RejectsNonHttpDocumentURLs) {
  for (const char* url :
       {"chrome://version/", "chrome-extension://test/", "mcbe://example.test/",
        "mcje://example.test/", "file:///test.html"}) {
    SCOPED_TRACE(url);
    SimRequest main("https://example.test/", "text/html");
    LoadURL("https://example.test/");
    main.Write("<body>");
    test::RunPendingTasks();
    // Retain the otherwise eligible origin and parser, isolating the URL gate.
    GetDocument().SetURL(KURL(url));
    main.Complete("<pyscript>forbidden scheme</pyscript>");
    test::RunPendingTasks();
    EXPECT_EQ(0u, runner_.contexts_created);
    EXPECT_EQ(0u, runner_.batches);
  }
}

TEST_F(HTMLPyScriptElementTest, RejectsAPIMutationBeforeClosingTag) {
  SimRequest main("https://example.test/", "text/html");
  LoadURL("https://example.test/");
  main.Write("<body><pyscript id=python>original");
  test::RunPendingTasks();
  ASSERT_TRUE(GetElementById("python"));
  GetElementById("python")->setTextContent("changed by API");
  main.Complete("tail</pyscript>");
  test::RunPendingTasks();
  EXPECT_EQ(0u, runner_.batches);
}

TEST_F(HTMLPyScriptElementTest, UnrestrictedPagesBypassOriginAndInlineCSP) {
  runner_.allow = false;
  runner_.unrestricted = true;
  SimRequest::Params params;
  params.response_http_headers.insert("Content-Security-Policy",
                                      "script-src 'none'; sandbox");
  SimRequest main("https://example.test/", "text/html", params);
  auto navigation = std::make_unique<WebNavigationParams>();
  navigation->url = KURL("https://example.test/");
  MockPolicyContainerHost policy_host;
  navigation->policy_container = std::make_unique<WebPolicyContainer>(
      WebPolicyContainerPolicies(),
      policy_host.BindNewEndpointAndPassDedicatedRemote());
  frame_test_helpers::FillNavigationParamsResponse(navigation.get());
  // Unlike SimTest::LoadURL(), supply the sandboxed origin that a real browser
  // computes before committing the navigation.
  navigation->origin_to_commit = SecurityOrigin::CreateUniqueOpaque();
  MainFrame().CommitNavigation(std::move(navigation), nullptr);
  main.Complete("<pyscript>explicit browser user override</pyscript>");
  test::RunPendingTasks();
  EXPECT_TRUE(Window().GetSecurityOrigin()->IsOpaque());
  EXPECT_EQ(1u, runner_.batches);
}

TEST_F(HTMLPyScriptElementTest, UnrestrictedPagesStillExcludePrivilegedURLs) {
  runner_.allow = false;
  runner_.unrestricted = true;
  for (const char* url : {"chrome://version/", "chrome-untrusted://test/",
                          "chrome-extension://test/", "mcbe://example.test/",
                          "mcje://example.test/"}) {
    SCOPED_TRACE(url);
    SimRequest main("https://example.test/", "text/html");
    LoadURL("https://example.test/");
    main.Write("<body>");
    test::RunPendingTasks();
    GetDocument().SetURL(KURL(url));
    main.Complete("<pyscript>forbidden scheme</pyscript>");
    test::RunPendingTasks();
    EXPECT_EQ(0u, runner_.contexts_created);
  }
}

TEST_F(HTMLPyScriptElementTest, UnrestrictedPagesStillExcludePrerender) {
  runner_.unrestricted = true;
  InitializePrerenderPageRoot();
  Load("<pyscript>prerender</pyscript>");
  EXPECT_EQ(0u, runner_.contexts_created);
}

TEST_F(HTMLPyScriptElementTest,
       UnrestrictedPagesRejectInheritedPrivilegedOrigin) {
  url::ScopedSchemeRegistryForTests scoped_registry;
  url::AddStandardScheme("chrome-extension", url::SCHEME_WITH_HOST);
  runner_.unrestricted = true;
  SimRequest main("https://example.test/", "text/html");
  LoadURL("https://example.test/");
  main.Write("<body>");
  test::RunPendingTasks();
  GetDocument().SetURL(KURL("about:blank"));
  scoped_refptr<SecurityOrigin> original_origin =
      Window().GetMutableSecurityOrigin();
  auto origin = SecurityOrigin::Create(KURL("chrome-extension://test/"));
  ASSERT_FALSE(origin->IsOpaque());
  GetDocument().domWindow()->GetSecurityContext().SetSecurityOriginForTesting(
      origin);
  main.Complete("<pyscript>forbidden inherited origin</pyscript>");
  test::RunPendingTasks();
  EXPECT_EQ(0u, runner_.contexts_created);
  // The Document outlives this test body. Restore its origin before the scoped
  // scheme registry is reset, so teardown can still compute its cookie site.
  Window().GetSecurityContext().SetSecurityOriginForTesting(original_origin);
}

TEST_F(HTMLPyScriptElementTest, RejectsRemovedAndAdoptedElements) {
  Load(R"HTML(<body><pyscript id=removed>removed</pyscript>
    <pyscript id=adopted>adopted</pyscript><script>
      document.querySelector('#removed').remove();
      globalThis.other = new DOMParser().parseFromString('', 'text/html');
      other.body.append(other.adoptNode(document.querySelector('#adopted')));
    </script>)HTML");
  EXPECT_EQ(0u, runner_.batches);
}

TEST_F(HTMLPyScriptElementTest, RejectsMissingCloseAndUnsupportedAttributes) {
  Load(R"HTML(<pyscript src="/never.py">external</pyscript>
    <pyscript async>async</pyscript><pyscript defer>defer</pyscript>
    <pyscript type="python">type</pyscript><pyscript>unclosed)HTML");
  EXPECT_EQ(0u, runner_.batches);
  EXPECT_GE(ConsoleMessages().size(), 5u);
}

TEST_F(HTMLPyScriptElementTest, DocumentWriteDisqualifiesWholeDocument) {
  Load(R"HTML(<pyscript>original</pyscript><script>
    document.write('<pyscript>written</pyscript>');
  </script><pyscript>later</pyscript>)HTML");
  EXPECT_EQ(0u, runner_.batches);
}

TEST_F(HTMLPyScriptElementTest, CSPNoneAndScriptSrcElemAreEnforced) {
  SimRequest::Params params;
  params.response_http_headers.insert("Content-Security-Policy",
                                      "script-src 'none'");
  Load("<pyscript>blocked</pyscript>", params);
  EXPECT_EQ(0u, runner_.batches);
  params.response_http_headers.Set(
      "Content-Security-Policy",
      "script-src 'unsafe-inline'; script-src-elem 'none'");
  Load("<pyscript>blocked</pyscript>", params);
  EXPECT_EQ(0u, runner_.batches);
}

TEST_F(HTMLPyScriptElementTest, NonceUsesOriginalSourceAndCurrentCSP) {
  SimRequest::Params params;
  params.response_http_headers.insert("Content-Security-Policy",
                                      "script-src 'nonce-allowed'");
  Load(R"HTML(<body><pyscript id=blocked nonce=wrong>blocked</pyscript>
             <pyscript id=accepted nonce=allowed>accepted</pyscript>
             <script nonce=allowed>
               document.querySelector('#blocked').nonce = 'allowed';
               const accepted = document.querySelector('#accepted');
               accepted.nonce = 'wrong'; accepted.textContent = 'changed';
             </script>)HTML",
       params);
  ASSERT_EQ(1u, runner_.executed.size());
  EXPECT_EQ("accepted", runner_.executed[0].source);
}

TEST_F(HTMLPyScriptElementTest, CSPAddedAfterBlockStillApplies) {
  Load(R"HTML(<head><pyscript>earlier block</pyscript>
      <meta http-equiv="Content-Security-Policy" content="script-src 'none'">
      </head><body>)HTML");
  EXPECT_EQ(0u, runner_.batches);
}

TEST_F(HTMLPyScriptElementTest, PrerenderDoesNotRunOrDeferPython) {
  InitializePrerenderPageRoot();
  Load("<pyscript>prerender</pyscript>");
  EXPECT_EQ(0u, runner_.batches);
}

TEST_F(HTMLPyScriptElementTest, DomBridgeWritesOnlyPlainTextAndFindsLightDom) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    EXPECT_TRUE(host.IsActive());
    auto handle = host.GetElementById("result");
    EXPECT_NE(0u, handle.handle.id);
    auto same = host.GetElementById("result");
    EXPECT_EQ(handle.handle.id, same.handle.id);
    EXPECT_EQ(
        WebPythonDomError::kNone,
        host.SetText(handle.handle, "42 <img src=never.png> &amp;").error);
    EXPECT_EQ("42 <img src=never.png> &amp;",
              GetElementById("result")->textContent());
    EXPECT_EQ(nullptr, GetDocument().QuerySelector(AtomicString("img")));
    EXPECT_EQ(0u, host.GetElementById("missing").handle.id);
    EXPECT_EQ(0u, host.GetElementById("shadow-child").handle.id);
    EXPECT_EQ(0u, host.GetElementById("template-child").handle.id);
    for (const char* id : {"div", "span"}) {
      EXPECT_EQ(
          WebPythonDomError::kNone,
          host.SetText(host.GetElementById(WebString::FromUtf8(id)).handle,
                       "text")
              .error);
    }
  });
  Load(R"HTML(<p id=result>before<!-- comment --></p><div id=div></div>
    <span id=span></span><div id=shadow></div>
    <template><p id=template-child>hidden</p></template>
    <script>document.querySelector('#shadow').attachShadow({mode:'closed'})
      .innerHTML='<p id="shadow-child">hidden</p>';</script>
    <pyscript>bridge</pyscript>)HTML");
}

TEST_F(HTMLPyScriptElementTest, ControlsPreserveNativeValuesAndSafeAttributes) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    auto range = host.GetElementById("range").handle;
    EXPECT_EQ(
        WebPythonDomError::kNone,
        host.UpdateElement(range, {WebPythonElementAction::kValue, {}, "999"})
            .error);
    EXPECT_EQ("420", host.GetControlState(range).value);
    auto box = host.GetElementById("box").handle;
    EXPECT_EQ(WebPythonDomError::kNone,
              host.UpdateElement(
                      box, {WebPythonElementAction::kChecked, {}, {}, true})
                  .error);
    EXPECT_TRUE(host.GetControlState(box).checked);
    auto panel = host.GetElementById("panel").handle;
    EXPECT_EQ(WebPythonDomError::kNone,
              host.UpdateElement(
                      panel, {WebPythonElementAction::kHidden, {}, {}, true})
                  .error);
    EXPECT_TRUE(host.GetControlState(panel).hidden);
    EXPECT_EQ(
        WebPythonDomError::kNone,
        host.UpdateElement(
                panel, {WebPythonElementAction::kClass, "selected", {}, true})
            .error);
    EXPECT_EQ("selected",
              GetElementById("panel")->getAttribute(html_names::kClassAttr));
    EXPECT_EQ(WebPythonDomError::kNone,
              host.UpdateElement(panel, {WebPythonElementAction::kAttribute,
                                         "aria-selected", "true"})
                  .error);
    EXPECT_EQ(WebPythonDomError::kNone,
              host.SetText(host.GetElementById("code").handle,
                           "<script>literal</script>")
                  .error);
    EXPECT_EQ("<script>literal</script>",
              GetElementById("code")->textContent());
  });
  Load(
      "<input id=range type=range min=160 max=420 value=280><input id=box "
      "type=checkbox>"
      "<div id=panel></div><code id=code></code><pyscript>controls</pyscript>");
}

TEST_F(HTMLPyScriptElementTest,
       ControlsRejectUnsafeNamesTypesAndExpiredHandles) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    auto panel = host.GetElementById("panel").handle;
    for (const char* name :
         {"onclick", "src", "href", "style", "is", "id", "innerHTML"}) {
      EXPECT_EQ(WebPythonDomError::kNotAllowed,
                host.UpdateElement(panel, {WebPythonElementAction::kAttribute,
                                           WebString::FromUtf8(name), "x"})
                    .error);
    }
    EXPECT_EQ(
        WebPythonDomError::kNotAllowed,
        host.UpdateElement(
                panel, {WebPythonElementAction::kClass, "two tokens", {}, true})
            .error);
    EXPECT_EQ(
        WebPythonDomError::kNotAllowed,
        host.UpdateElement(
                panel, {WebPythonElementAction::kProgress, {}, {}, false, 101})
            .error);
    for (const char* id : {"file", "password"}) {
      auto h = host.GetElementById(WebString::FromUtf8(id)).handle;
      EXPECT_EQ(WebPythonDomError::kNotAllowed,
                host.GetControlState(h).status.error);
      EXPECT_EQ(WebPythonDomError::kNotAllowed,
                host.UpdateElement(h, {WebPythonElementAction::kValue, {}, "x"})
                    .error);
    }
    EXPECT_EQ(
        WebPythonDomError::kNotAllowed,
        host.UpdateElement(host.GetElementById("custom").handle,
                           {WebPythonElementAction::kHidden, {}, {}, true})
            .error);
    ++panel.generation;
    EXPECT_EQ(WebPythonDomError::kReference,
              host.GetControlState(panel).status.error);
    EXPECT_EQ(WebPythonDomError::kReference,
              host.UpdateElement(
                      panel, {WebPythonElementAction::kHidden, {}, {}, true})
                  .error);
    panel = host.GetElementById("panel").handle;
    GetElementById("panel")->remove();
    EXPECT_EQ(
        WebPythonDomError::kReference,
        host.UpdateElement(panel, {WebPythonElementAction::kFocus}).error);
  });
  Load(
      "<div id=panel></div><input id=file type=file><input id=password "
      "type=password>"
      "<div id=custom is=x-ui></div><pyscript>controls</pyscript>");
}

TEST_F(HTMLPyScriptElementTest,
       ControlsUseNativeModalStateAndDisabledSemantics) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    auto dialog = host.GetElementById("dialog").handle;
    EXPECT_EQ(
        WebPythonDomError::kNone,
        host.UpdateElement(dialog, {WebPythonElementAction::kShowModal}).error);
    EXPECT_TRUE(host.GetControlState(dialog).open);
    EXPECT_EQ(WebPythonDomError::kNone,
              host.UpdateElement(dialog, {WebPythonElementAction::kCloseDialog})
                  .error);
    EXPECT_FALSE(host.GetControlState(dialog).open);
    auto button = host.GetElementById("button").handle;
    EXPECT_EQ(WebPythonDomError::kNone,
              host.UpdateElement(
                      button, {WebPythonElementAction::kDisabled, {}, {}, true})
                  .error);
    EXPECT_TRUE(host.GetControlState(button).disabled);
  });
  Load(
      "<dialog id=dialog><button>close</button></dialog><button "
      "id=button>play</button>"
      "<pyscript>controls</pyscript>");
}

TEST_F(HTMLPyScriptElementTest,
       SkippedUiNotificationsDoNotInvalidateHeldInput) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    auto doc = host.GetEventTarget(WebPythonTargetKind::kDocument).handle;
    auto button = host.GetElementById("button").handle;
    auto dialog = host.GetElementById("dialog").handle;
    EXPECT_EQ(WebPythonDomError::kNone,
              host.AddEventListener(button, "click", {1, 1}, false).error);
    EXPECT_EQ(WebPythonDomError::kNone,
              host.AddEventListener(doc, "focusin", {1, 2}, false).error);
    uint64_t callback_id = 3;
    for (const char* type : {"close", "cancel"}) {
      EXPECT_EQ(WebPythonDomError::kNone,
                host.AddEventListener(dialog, WebString::FromUtf8(type),
                                      {1, callback_id++}, false)
                    .error);
    }
  });
  Load(
      "<body><button id=button>play</button><dialog id=dialog></dialog>"
      "<pyscript>notifications</pyscript>");
  runner_.on_click = base::BindLambdaForTesting([&] {
    const auto before = runner_.host->GetPageState().state.input_epoch;
    GetElementById("button")->DispatchEvent(
        *Event::CreateBubble(event_type_names::kFocusin));
    for (const auto& type :
         {event_type_names::kClose, event_type_names::kCancel}) {
      GetElementById("dialog")->DispatchEvent(*Event::CreateBubble(type));
    }
    EXPECT_EQ(before, runner_.host->GetPageState().state.input_epoch);
  });
  Click();
  EXPECT_EQ((std::vector<uint64_t>{1}), runner_.invoked);
}

TEST_F(HTMLPyScriptElementTest, DomBridgeRejectsCodeCustomAndContainerTargets) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    for (const char* id : {"script", "python", "style", "custom", "customized",
                           "pending-is", "container", "svg", "foreign"}) {
      const auto handle = host.GetElementById(WebString::FromUtf8(id)).handle;
      ASSERT_NE(0u, handle.id) << id;
      EXPECT_EQ(WebPythonDomError::kNotAllowed,
                host.SetText(handle, "deny").error)
          << id;
    }
  });
  Load(R"HTML(<script id=script></script><style id=style></style>
    <x-python id=custom></x-python><p is=x-p id=customized>keep</p>
    <div is=not-defined id=pending-is>keep</div>
    <div id=container><span>keep</span></div>
    <svg id=svg><g id=foreign></g></svg>
    <script>customElements.define('x-python', class extends HTMLElement {});
      customElements.define('x-p', class extends HTMLParagraphElement {},
        {extends:'p'});</script><pyscript id=python>bridge</pyscript>)HTML");
  EXPECT_EQ("keep", GetElementById("container")->textContent());
}

TEST_F(HTMLPyScriptElementTest,
       DomBridgeRejectsForeignExpiredAndRemovedHandles) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    auto handle = host.GetElementById("result").handle;
    auto foreign = handle;
    ++foreign.document;
    EXPECT_EQ(WebPythonDomError::kReference,
              host.SetText(foreign, "deny").error);
    foreign = handle;
    ++foreign.generation;
    EXPECT_EQ(WebPythonDomError::kReference,
              host.SetText(foreign, "deny").error);
    foreign = handle;
    foreign.id = 0;
    EXPECT_EQ(WebPythonDomError::kReference,
              host.SetText(foreign, "deny").error);
    GetElementById("result")->remove();
    EXPECT_EQ(WebPythonDomError::kReference,
              host.SetText(handle, "deny").error);
    GetDocument().body()->SetInnerHTMLWithoutTrustedTypes(
        "<p id=result>replacement</p>");
    EXPECT_EQ(WebPythonDomError::kReference,
              host.SetText(handle, "deny").error);
    EXPECT_EQ("replacement", GetElementById("result")->textContent());
  });
  Load("<body><p id=result>before</p><pyscript>bridge</pyscript>");
}

TEST_F(HTMLPyScriptElementTest, DomBridgeHasBoundedStringsAndDocumentCalls) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    auto handle = host.GetElementById("result").handle;
    EXPECT_EQ(WebPythonDomError::kLimit,
              host.SetText(handle, WebString::FromUtf8(std::string(16385, 'x')))
                  .error);
    EXPECT_EQ("before", GetElementById("result")->textContent());
    for (unsigned i = 2; i < 1024; ++i) {
      EXPECT_EQ(WebPythonDomError::kNone, host.GetElementById("missing").error);
    }
    EXPECT_EQ(WebPythonDomError::kStopped, host.SetText(handle, "deny").error);
    EXPECT_FALSE(host.IsActive());
    EXPECT_EQ(WebPythonDomError::kStopped, host.GetElementById("result").error);
    EXPECT_EQ(WebPythonDomError::kStopped,
              host.SetText(handle, "still denied").error);
  });
  Load("<p id=result>before</p><pyscript>bridge</pyscript>");
  EXPECT_EQ(1u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest, ListenerRegistrationAndRemovalCountDomCalls) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    const auto element = host.GetElementById("button").handle;
    const WebPythonCallbackHandle callback{1, 1};
    EXPECT_EQ(WebPythonDomError::kNone,
              host.AddEventListener(element, "click", callback, true).error);
    EXPECT_EQ(WebPythonDomError::kNone,
              host.RemoveEventListener(element, callback).error);
    host.RevokeEventListener(element, callback);
    for (unsigned i = 3; i < 1024; ++i) {
      EXPECT_EQ(WebPythonDomError::kNone, host.GetElementById("missing").error);
    }
    EXPECT_EQ(WebPythonDomError::kStopped,
              host.AddEventListener(element, "click", callback, false).error);
    EXPECT_EQ(WebPythonDomError::kStopped,
              host.RemoveEventListener(element, callback).error);
    // Internal rollback does not need permission or consume another DOM call.
    host.RevokeEventListener(element, callback);
    EXPECT_FALSE(host.IsActive());
  });
  Load("<button id=button>button</button><pyscript>bridge</pyscript>");
  EXPECT_EQ(1u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest,
       NativeFramesConsumeBeforeEntryAndRescheduleLater) {
  RegisterFrame();
  runner_.on_frame = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    EXPECT_TRUE(host.IsActive());
    EXPECT_EQ(
        WebPythonDomError::kNone,
        host.SetText(host.GetElementById("result").handle, "frame").error);
    if (runner_.frames.size() == 1) {
      EXPECT_EQ(WebPythonDomError::kNone, host.RequestFrame({1, 2}).error);
    }
  });
  EXPECT_TRUE(runner_.frames.empty());
  Compositor().BeginFrame();
  EXPECT_EQ((std::vector<uint64_t>{1}), runner_.frames);
  Compositor().BeginFrame();
  EXPECT_EQ((std::vector<uint64_t>{1, 2}), runner_.frames);
  EXPECT_LT(runner_.timestamps[0], runner_.timestamps[1]);
  EXPECT_EQ("frame", GetElementById("result")->textContent());
  EXPECT_FALSE(
      GetDocument().GetScriptedAnimationController().HasFrameCallback());
}

TEST_F(HTMLPyScriptElementTest,
       FrameRegistrationIsBoundedAndCancelIsIdempotent) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    EXPECT_EQ(WebPythonDomError::kState, host.RequestFrame({}).error);
    EXPECT_EQ(WebPythonDomError::kNone, host.RequestFrame({1, 1}).error);
    EXPECT_EQ(WebPythonDomError::kState, host.RequestFrame({1, 2}).error);
    EXPECT_EQ(WebPythonDomError::kNone, host.CancelFrame({2, 1}).error);
    EXPECT_EQ(WebPythonDomError::kNone, host.CancelFrame({1, 1}).error);
    EXPECT_EQ(WebPythonDomError::kNone, host.CancelFrame({1, 1}).error);
    EXPECT_EQ(WebPythonDomError::kNone, host.RequestFrame({1, 3}).error);
  });
  Load("<pyscript>register</pyscript>");
  Tick(10);
  Tick(20);
  EXPECT_EQ((std::vector<uint64_t>{3}), runner_.frames);
}

TEST_F(HTMLPyScriptElementTest, OldFrameHandleDoesNotCancelNewRequest) {
  RegisterFrame();
  runner_.on_frame = base::BindLambdaForTesting([&] {
    if (runner_.frames.size() == 1) {
      EXPECT_EQ(WebPythonDomError::kNone,
                runner_.host->RequestFrame({1, 2}).error);
      EXPECT_EQ(WebPythonDomError::kNone,
                runner_.host->CancelFrame({1, 1}).error);
    }
  });
  Tick(10);
  Tick(20);
  EXPECT_EQ((std::vector<uint64_t>{1, 2}), runner_.frames);
}

TEST_F(HTMLPyScriptElementTest,
       FrameWindowRefusalRetainsRequestWithFreshTimestamp) {
  RegisterFrame();
  runner_.frame_admitted = false;
  Tick(10);
  Tick(20);
  EXPECT_TRUE(runner_.frames.empty());
  EXPECT_TRUE(
      GetDocument().GetScriptedAnimationController().HasFrameCallback());
  runner_.frame_admitted = true;
  Tick(30);
  Tick(40);
  EXPECT_EQ((std::vector<uint64_t>{1}), runner_.frames);
  EXPECT_EQ((std::vector<double>{30}), runner_.timestamps);
}

TEST_F(HTMLPyScriptElementTest,
       ScriptForbiddenFrameRetainsRequestAndSurvivesGC) {
  RegisterFrame();
  {
    ScriptForbiddenScope forbidden;
    Tick(10);
  }
  EXPECT_TRUE(runner_.frames.empty());
  ThreadState::Current()->CollectAllGarbageForTesting();
  Tick(20);
  EXPECT_EQ((std::vector<uint64_t>{1}), runner_.frames);
  EXPECT_EQ((std::vector<double>{20}), runner_.timestamps);
}

TEST_F(HTMLPyScriptElementTest, HiddenFramesPauseAndResumeWithoutCatchUp) {
  RegisterFrame();
  GetDocument().GetPage()->SetVisibilityState(
      mojom::blink::PageVisibilityState::kHidden, false);
  Tick(10);
  Tick(20);
  EXPECT_TRUE(runner_.frames.empty());
  GetDocument().GetPage()->SetVisibilityState(
      mojom::blink::PageVisibilityState::kVisible, false);
  Tick(5000);
  Tick(5016);
  EXPECT_EQ((std::vector<uint64_t>{1}), runner_.frames);
  EXPECT_EQ((std::vector<double>{5000}), runner_.timestamps);
}

TEST_F(HTMLPyScriptElementTest, FrameSourceNonceSurvivesDetachedTagCollection) {
  runner_.frame_admitted = false;
  runner_.on_run = base::BindLambdaForTesting([&] {
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host->RequestFrame({1, 1}).error);
  });
  SimRequest::Params params;
  params.response_http_headers.insert("Content-Security-Policy",
                                      "script-src 'nonce-frame'");
  Load("<body><pyscript id=python nonce=frame>register</pyscript>", params);
  Compositor().BeginFrame();
  // A pending frame must not retain the original element.
  WeakPersistent<Element> source(GetElementById("python"));
  source->remove();
  Compositor().BeginFrame();
  ThreadState::Current()->CollectAllGarbageForTesting();
  EXPECT_FALSE(source);
  EXPECT_TRUE(runner_.frames.empty());
  runner_.frame_admitted = true;
  Tick(100);
  EXPECT_EQ((std::vector<uint64_t>{1}), runner_.frames);
}

TEST_F(HTMLPyScriptElementTest, RevokedFramePolicyStopsRatherThanRevives) {
  RegisterFrame();
  runner_.allow = false;
  Tick(10);
  test::RunPendingTasks();
  runner_.allow = true;
  Tick(20);
  EXPECT_TRUE(runner_.frames.empty());
  EXPECT_EQ(1u, *runner_.destructions);
  EXPECT_FALSE(
      GetDocument().GetScriptedAnimationController().HasFrameCallback());
}

TEST_F(HTMLPyScriptElementTest, FrameRechecksLateCSPAndUnrestrictedOverride) {
  RegisterFrame();
  auto* meta = GetDocument().CreateRawElement(html_names::kMetaTag);
  meta->setAttribute(html_names::kHttpEquivAttr,
                     AtomicString("Content-Security-Policy"));
  meta->setAttribute(html_names::kContentAttr,
                     AtomicString("script-src 'none'"));
  GetDocument().head()->AppendChild(meta);
  runner_.unrestricted = true;
  runner_.on_frame = base::BindLambdaForTesting([&] {
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host->RequestFrame({1, 2}).error);
  });
  Tick(10);
  EXPECT_EQ(1u, runner_.frames.size());
  runner_.unrestricted = false;
  Tick(20);
  test::RunPendingTasks();
  EXPECT_EQ(1u, runner_.frames.size());
  EXPECT_EQ(1u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest, FrameErrorKeepsNewRequestFatalRevokesAll) {
  RegisterFrame();
  runner_.on_frame = base::BindLambdaForTesting([&] {
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host->RequestFrame({1, runner_.frames.back() + 1}).error);
  });
  runner_.callback_result = {false, false, {}, {}, "ValueError: frame"};
  Tick(10);
  Tick(20);
  EXPECT_EQ((std::vector<uint64_t>{1, 2}), runner_.frames);
  EXPECT_EQ(0u, *runner_.destructions);
  runner_.callback_result.stopped = true;
  Tick(30);
  test::RunPendingTasks();
  Tick(40);
  EXPECT_EQ(3u, runner_.frames.size());
  EXPECT_EQ(1u, *runner_.destructions);
  EXPECT_TRUE(ConsoleMessages().back().contains("pyscript frame 3"));
}

TEST_F(HTMLPyScriptElementTest, FrameOpenRevokesPendingOutputAndDefersCleanup) {
  RegisterFrame();
  runner_.callback_result.stdout_text = "must-not-log-frame-after-open";
  runner_.on_frame = base::BindLambdaForTesting([&] {
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host->RequestFrame({1, 2}).error);
    GetDocument().open();
    EXPECT_FALSE(runner_.host->IsActive());
    EXPECT_EQ(0u, *runner_.destructions);
  });
  Tick(10);
  test::RunPendingTasks();
  Tick(20);
  EXPECT_EQ(1u, runner_.frames.size());
  EXPECT_EQ(1u, *runner_.destructions);
  for (const auto& message : ConsoleMessages()) {
    EXPECT_FALSE(message.contains("must-not-log-frame-after-open"));
  }
}

TEST_F(HTMLPyScriptElementTest,
       NavigationCancelsPendingFrameBeforePythonEntry) {
  RegisterFrame();
  runner_.on_run.Reset();
  Load("<p>new document</p>");
  Tick(10);
  test::RunPendingTasks();
  EXPECT_TRUE(runner_.frames.empty());
  EXPECT_EQ(1u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest, FrameBridgeHasItsOwnPerEntryDomAllowance) {
  RegisterFrame();
  runner_.on_frame = base::BindLambdaForTesting([&] {
    // Registration itself counts as one of the 128 bridge calls.
    EXPECT_EQ(WebPythonDomError::kNone,
              runner_.host->RequestFrame({1, runner_.frames.back() + 1}).error);
    for (int i = 0; i < 127; ++i) {
      EXPECT_EQ(WebPythonDomError::kNone,
                runner_.host->GetElementById("missing").error);
    }
  });
  Tick(10);
  Tick(20);
  EXPECT_EQ(2u, runner_.frames.size());
  runner_.on_frame = base::BindLambdaForTesting([&] {
    for (int i = 0; i < 129; ++i) {
      EXPECT_EQ(
          i == 128 ? WebPythonDomError::kStopped : WebPythonDomError::kNone,
          runner_.host->GetElementById("missing").error);
    }
  });
  Tick(30);
  test::RunPendingTasks();
  EXPECT_EQ(1u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest, NativeClickBubblesAndUsesDocumentHost) {
  RegisterClick();
  runner_.on_click = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    EXPECT_TRUE(host.IsActive());
    EXPECT_EQ(
        WebPythonDomError::kNone,
        host.SetText(host.GetElementById("result").handle, "clicked").error);
  });
  Click("child");
  EXPECT_FALSE(runner_.last_event.is_trusted);
  Click("button", true);
  EXPECT_EQ((std::vector<uint64_t>{1, 1}), runner_.invoked);
  EXPECT_EQ("click", runner_.last_event.type);
  EXPECT_TRUE(runner_.last_event.is_trusted);
  EXPECT_EQ("clicked", GetElementById("result")->textContent());
}

TEST_F(HTMLPyScriptElementTest, OnceConsumesBeforeCallbackEvenOnError) {
  RegisterClick(true);
  runner_.callback_result = {false, false, {}, {}, "ValueError: once"};
  runner_.on_click = base::BindLambdaForTesting([&] { Click(); });
  Click();
  Click();
  EXPECT_EQ((std::vector<uint64_t>{1}), runner_.invoked);
  EXPECT_TRUE(ConsoleMessages().back().contains("callback 1"));
  EXPECT_EQ(0u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest, WindowRefusalDoesNotConsumeOnceOrQueueClicks) {
  RegisterClick(true);
  runner_.callback_admitted = false;
  const auto messages = ConsoleMessages().size();
  Click();
  Click();
  test::RunPendingTasks();
  EXPECT_TRUE(runner_.invoked.empty());
  EXPECT_EQ(messages + 1, ConsoleMessages().size());
  runner_.callback_admitted = true;
  test::RunPendingTasks();
  EXPECT_TRUE(runner_.invoked.empty());
  Click();
  Click();
  EXPECT_EQ(1u, runner_.invoked.size());
}

TEST_F(HTMLPyScriptElementTest,
       CallbackDomCallsDoNotSpendInitializationAllowance) {
  RegisterClick();
  runner_.on_click = base::BindLambdaForTesting([&] {
    for (int i = 0; i < 128; ++i) {
      EXPECT_EQ(WebPythonDomError::kNone,
                runner_.host->GetElementById("missing").error);
    }
  });
  for (int i = 0; i < 10; ++i) {
    Click();
  }
  EXPECT_EQ(10u, runner_.invoked.size());
  runner_.on_click = base::BindLambdaForTesting([&] {
    for (int i = 0; i < 129; ++i) {
      const auto result = runner_.host->GetElementById("missing");
      EXPECT_EQ(
          i == 128 ? WebPythonDomError::kStopped : WebPythonDomError::kNone,
          result.error);
    }
  });
  Click();
  test::RunPendingTasks();
  Click();
  EXPECT_EQ(11u, runner_.invoked.size());
  EXPECT_EQ(1u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest,
       CallbackOutputDoesNotSpendInitializationAllowance) {
  RegisterClick();
  runner_.callback_result.stdout_text =
      String::FromUtf8(std::string(3000, 'x'));
  for (int i = 0; i < 8; ++i) {
    Click();
  }
  EXPECT_EQ(8u, runner_.invoked.size());
  EXPECT_EQ(0u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest,
       DocumentOpenDuringCallbackClosesOutputAndDefersCleanup) {
  RegisterClick();
  Persistent<Element> old_button(GetElementById("button"));
  runner_.callback_result.stdout_text = "must-not-log-after-invalidation";
  runner_.on_click = base::BindLambdaForTesting([&] {
    GetDocument().open();
    EXPECT_FALSE(runner_.host->IsActive());
    EXPECT_EQ(WebPythonDomError::kReference,
              runner_.host->GetElementById("missing").error);
    EXPECT_EQ(0u, *runner_.destructions);
  });
  Click();
  test::RunPendingTasks();
  old_button->DispatchEvent(*Event::CreateBubble(event_type_names::kClick));
  EXPECT_EQ(1u, runner_.invoked.size());
  EXPECT_EQ(1u, *runner_.destructions);
  for (const auto& message : ConsoleMessages()) {
    EXPECT_FALSE(message.contains("must-not-log-after-invalidation"));
  }
}

TEST_F(HTMLPyScriptElementTest,
       OrdinaryCallbackErrorRetainsListenerFatalStopsAll) {
  RegisterClick();
  runner_.callback_result = {false, false, {}, {}, "ValueError: ordinary"};
  Click();
  Click();
  EXPECT_EQ(2u, runner_.invoked.size());
  runner_.callback_result.stopped = true;
  Click();
  test::RunPendingTasks();
  Click();
  EXPECT_EQ(3u, runner_.invoked.size());
  EXPECT_EQ(1u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest,
       CallbackCanRemoveLaterListenerAndAddForNextClick) {
  WebPythonDomHandle target;
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    target = host.GetElementById("button").handle;
    for (uint64_t id : {1, 2}) {
      EXPECT_EQ(WebPythonDomError::kNone,
                host.AddEventListener(target, "click", {1, id}, false).error);
    }
  });
  Load("<button id=button>click</button><pyscript>register</pyscript>");
  runner_.on_click = base::BindLambdaForTesting([&] {
    if (runner_.invoked.back() != 1) {
      return;
    }
    auto& host = *runner_.host;
    EXPECT_EQ(WebPythonDomError::kNone,
              host.RemoveEventListener(target, {1, 2}).error);
    EXPECT_EQ(WebPythonDomError::kNone,
              host.RemoveEventListener(target, {1, 1}).error);
    EXPECT_EQ(WebPythonDomError::kNone,
              host.AddEventListener(target, "click", {1, 3}, false).error);
  });
  Click();
  EXPECT_EQ((std::vector<uint64_t>{1}), runner_.invoked);
  Click();
  EXPECT_EQ((std::vector<uint64_t>{1, 3}), runner_.invoked);
}

TEST_F(HTMLPyScriptElementTest,
       DetachedNodesSkipReconnectResumesReplacementDoesNot) {
  RegisterClick();
  Persistent<Element> original(GetElementById("button"));
  original->remove();
  original->DispatchEvent(*Event::CreateBubble(event_type_names::kClick));
  EXPECT_TRUE(runner_.invoked.empty());
  GetDocument().body()->AppendChild(original);
  Click();
  EXPECT_EQ(1u, runner_.invoked.size());
  original->remove();
  auto* replacement = GetDocument().CreateRawElement(html_names::kButtonTag);
  replacement->setAttribute(html_names::kIdAttr, AtomicString("button"));
  GetDocument().body()->AppendChild(replacement);
  Click();
  EXPECT_EQ(1u, runner_.invoked.size());
}

TEST_F(HTMLPyScriptElementTest,
       ClickRechecksPolicyAndDoesNotRequireSourceNode) {
  RegisterClick(true);
  GetElementById("python")->remove();
  runner_.allow = false;
  Click();
  EXPECT_TRUE(runner_.invoked.empty());
  runner_.allow = true;
  Click();
  EXPECT_EQ(1u, runner_.invoked.size());
}

TEST_F(HTMLPyScriptElementTest, ClickRechecksCurrentCSPWithCopiedNonce) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    EXPECT_EQ(WebPythonDomError::kNone,
              host.AddEventListener(host.GetElementById("button").handle,
                                    "click", {1, 1}, false)
                  .error);
  });
  SimRequest::Params params;
  params.response_http_headers.insert("Content-Security-Policy",
                                      "script-src 'nonce-click'");
  Load(
      "<body><button id=button>click</button>"
      "<pyscript nonce=click id=python>register</pyscript>",
      params);
  GetElementById("python")->remove();
  Click();
  EXPECT_EQ(1u, runner_.invoked.size());
}

TEST_F(HTMLPyScriptElementTest,
       LateCSPAndUnrestrictedOverrideApplyToOldListeners) {
  RegisterClick();
  auto* meta = GetDocument().CreateRawElement(html_names::kMetaTag);
  meta->setAttribute(html_names::kHttpEquivAttr,
                     AtomicString("Content-Security-Policy"));
  meta->setAttribute(html_names::kContentAttr,
                     AtomicString("script-src 'none'"));
  GetDocument().head()->AppendChild(meta);
  Click();
  EXPECT_TRUE(runner_.invoked.empty());
  runner_.unrestricted = true;
  Click();
  EXPECT_EQ(1u, runner_.invoked.size());
}

TEST_F(HTMLPyScriptElementTest, NestedClickIsSkippedWithoutQueuing) {
  RegisterClick();
  runner_.on_click = base::BindLambdaForTesting([&] { Click(); });
  Click();
  test::RunPendingTasks();
  EXPECT_EQ(1u, runner_.invoked.size());
}

TEST_F(HTMLPyScriptElementTest, ScriptForbiddenClickDoesNotConsumeOnce) {
  RegisterClick(true);
  {
    ScriptForbiddenScope forbidden;
    Click();
  }
  EXPECT_TRUE(runner_.invoked.empty());
  Click();
  Click();
  EXPECT_EQ(1u, runner_.invoked.size());
}

TEST_F(HTMLPyScriptElementTest, InvalidationDuringCallbackDefersDestruction) {
  RegisterClick();
  runner_.on_click = base::BindLambdaForTesting([&] {
    PythonScriptController::StopDocument(GetDocument());
    EXPECT_FALSE(runner_.host->IsActive());
    EXPECT_EQ(0u, *runner_.destructions);
  });
  Click();
  test::RunPendingTasks();
  Click();
  EXPECT_EQ(1u, runner_.invoked.size());
  EXPECT_EQ(1u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest, SubscriptionsDoNotRetainDetachedNodes) {
  RegisterClick();
  Compositor().BeginFrame();
  WeakPersistent<Element> button(GetElementById("button"));
  WeakPersistent<Element> source(GetElementById("python"));
  button->remove();
  source->remove();
  Compositor().BeginFrame();
  test::RunPendingTasks();
  ThreadState::Current()->CollectAllGarbageForTesting();
  EXPECT_FALSE(button);
  EXPECT_FALSE(source);
  test::RunPendingTasks();
  EXPECT_EQ(0u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest, FatalResultAllowsPrefixAndReleasesContext) {
  runner_.supplied_results = std::vector<WebPythonScriptResult>{
      {false, false, "before stop", {}, "cooperative timeout", true}};
  Load("<pyscript>loop</pyscript><pyscript>must not run</pyscript>");
  EXPECT_EQ(1u, *runner_.destructions);
  ASSERT_EQ(2u, ConsoleMessages().size());
  EXPECT_EQ("before stop", ConsoleMessages()[0]);
  EXPECT_TRUE(ConsoleMessages()[1].contains("cooperative timeout"));
}

TEST_F(HTMLPyScriptElementTest, OrdinaryErrorRetainsContextAndLaterResults) {
  runner_.supplied_results = std::vector<WebPythonScriptResult>{
      {false, false, {}, {}, "ValueError: ordinary"},
      {true, false, "later block", {}, {}}};
  Load("<pyscript>error</pyscript><pyscript>later</pyscript>");
  EXPECT_EQ(0u, *runner_.destructions);
  ASSERT_EQ(2u, ConsoleMessages().size());
  EXPECT_EQ("later block", ConsoleMessages()[1]);
}

TEST_F(HTMLPyScriptElementTest,
       ConsoleOutputAndErrorsHaveOneUtf8DocumentLimit) {
  runner_.supplied_results = std::vector<WebPythonScriptResult>{
      {true, false, WebString::FromUtf8(std::string(9000, 'x')), {}, {}},
      {false,
       false,
       {},
       WebString::FromUtf8(std::string(9000, 'y')),
       "error that no longer fits"}};
  Load("<pyscript>stdout</pyscript><pyscript>stderr</pyscript>");
  size_t bytes = 0;
  unsigned notices = 0;
  for (const auto& message : ConsoleMessages()) {
    bytes += message.Utf8().size();
    notices += message.contains("output was truncated at 16 KiB");
  }
  EXPECT_LE(bytes, 16384u);
  EXPECT_EQ(1u, notices);
  EXPECT_EQ(1u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest, ParserDiagnosticsShareDocumentConsoleLimit) {
  StringBuilder html;
  for (unsigned i = 0; i < 600; ++i) {
    html.Append("<pyscript src=unsupported>ignored</pyscript>");
  }
  Load(html.ToString());
  size_t bytes = 0;
  unsigned notices = 0;
  for (const auto& message : ConsoleMessages()) {
    bytes += message.Utf8().size();
    notices += message.contains("output was truncated at 16 KiB");
  }
  EXPECT_LE(bytes, 16384u);
  EXPECT_EQ(1u, notices);
  EXPECT_EQ(0u, runner_.contexts_created);
}

TEST_F(HTMLPyScriptElementTest, CleanupTaskRechecksForbiddenScope) {
  Load("<pyscript>bridge</pyscript>");
  {
    ScriptForbiddenScope forbid;
    PythonScriptController::StopDocument(GetDocument());
    test::RunPendingTasks();
    EXPECT_EQ(0u, *runner_.destructions);
  }
  test::RunDelayedTasks(base::Milliseconds(10));
  EXPECT_EQ(1u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest,
       ContextPersistsUntilDocumentOpenAndCleansInTask) {
  WebPythonDomHost* host = nullptr;
  runner_.on_run = base::BindLambdaForTesting([&] { host = runner_.host; });
  Load("<pyscript>bridge</pyscript>");
  ASSERT_TRUE(host);
  EXPECT_FALSE(host->IsActive());
  EXPECT_EQ(WebPythonDomError::kReference, host->GetElementById("x").error);
  EXPECT_EQ(1u, runner_.contexts_created);
  EXPECT_EQ(0u, *runner_.destructions);
  EXPECT_TRUE(GetDocument()
                  .GetFrame()
                  ->GetFrameScheduler()
                  ->GetActiveFeaturesTrackedForBackForwardCacheMetrics()
                  .Contains(SchedulingPolicy::Feature::kNativePython));
  GetDocument().open();
  EXPECT_EQ(0u, *runner_.destructions);
  test::RunPendingTasks();
  EXPECT_EQ(1u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest, InvalidationDuringRunDefersContextDestruction) {
  runner_.on_run = base::BindLambdaForTesting([&] {
    auto& host = *runner_.host;
    auto handle = host.GetElementById("result").handle;
    PythonScriptController::StopDocument(GetDocument());
    EXPECT_EQ(0u, *runner_.destructions);
    EXPECT_FALSE(host.IsActive());
    EXPECT_EQ(WebPythonDomError::kReference,
              host.SetText(handle, "deny").error);
  });
  Load("<p id=result>before</p><pyscript>bridge</pyscript>");
  EXPECT_EQ(1u, *runner_.destructions);
  EXPECT_EQ("before", GetElementById("result")->textContent());
}

TEST_F(HTMLPyScriptElementTest, NavigationDisposesOldDocumentContext) {
  Load("<pyscript>bridge</pyscript>");
  EXPECT_EQ(0u, *runner_.destructions);
  Load("<p>next document</p>");
  EXPECT_EQ(1u, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest, GarbageCollectionAndNavigationReleaseContexts) {
  for (unsigned i = 0; i < 4; ++i) {
    Load("<pyscript>keep live document</pyscript>");
    ThreadState::Current()->CollectAllGarbageForTesting();
    test::RunPendingTasks();
    EXPECT_EQ(i, *runner_.destructions);
    Load("<p>replacement</p>");
    ThreadState::Current()->CollectAllGarbageForTesting();
    test::RunPendingTasks();
    EXPECT_EQ(i + 1, *runner_.destructions);
  }
  EXPECT_EQ(runner_.contexts_created, *runner_.destructions);
}

TEST_F(HTMLPyScriptElementTest,
       ClosingFrameReleasesContextOutsideLifecycleStack) {
  Load("<pyscript>close document</pyscript>");
  EXPECT_EQ(0u, *runner_.destructions);
  WebViewHelper().Reset();
  test::RunPendingTasks();
  EXPECT_EQ(1u, *runner_.destructions);
}

}  // namespace
}  // namespace blink
