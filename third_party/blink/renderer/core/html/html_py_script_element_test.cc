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
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/core/event_type_names.h"
#include "third_party/blink/renderer/core/frame/csp/content_security_policy.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/html/html_head_element.h"
#include "third_party/blink/renderer/core/html/html_unknown_element.h"
#include "third_party/blink/renderer/core/html/python_script_controller.h"
#include "third_party/blink/renderer/core/html_names.h"
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
  unsigned contexts_created = 0;
  std::shared_ptr<unsigned> destructions = std::make_shared<unsigned>(0);
  raw_ptr<WebPythonDomHost> host = nullptr;
  unsigned batches = 0;
  base::RepeatingClosure on_run;
  base::RepeatingClosure on_click;
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
  ScopedPythonScriptForTest enabled_{true};
  RecordingPythonRunner runner_;
};

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
