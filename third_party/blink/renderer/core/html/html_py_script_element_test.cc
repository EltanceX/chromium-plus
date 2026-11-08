// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/html_py_script_element.h"

#include "base/memory/raw_ref.h"
#include "base/test/bind.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/web/web_python_script_runner.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/html/html_unknown_element.h"
#include "third_party/blink/renderer/core/html/python_script_controller.h"
#include "third_party/blink/renderer/core/testing/sim/sim_request.h"
#include "third_party/blink/renderer/core/testing/sim/sim_test.h"
#include "third_party/blink/renderer/platform/scheduler/public/frame_scheduler.h"
#include "third_party/blink/renderer/platform/testing/runtime_enabled_features_test_helpers.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"

namespace blink {
namespace {

class RecordingPythonRunner final : public WebPythonScriptRunner {
 public:
  class Context final : public WebPythonScriptContext {
   public:
    explicit Context(RecordingPythonRunner& runner)
        : runner_(runner), destructions_(runner.destructions) {}
    ~Context() override { ++*destructions_; }
    std::vector<WebPythonScriptResult> Run(
        const std::vector<WebPythonScriptSource>& sources,
        WebPythonDomHost& dom_host) override {
      runner_->host = &dom_host;
      auto results = runner_->Run(sources);
      runner_->host = nullptr;
      return results;
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
  std::vector<WebPythonScriptResult> Run(
      const std::vector<WebPythonScriptSource>& sources) {
    ++batches;
    executed = sources;
    if (on_run) {
      on_run.Run();
    }
    std::vector<WebPythonScriptResult> results(sources.size());
    for (auto& result : results) {
      result.success = true;
    }
    return results;
  }
  bool allow = true;
  unsigned contexts_created = 0;
  std::shared_ptr<unsigned> destructions = std::make_shared<unsigned>(0);
  raw_ptr<WebPythonDomHost> host = nullptr;
  unsigned batches = 0;
  base::RepeatingClosure on_run;
  std::vector<WebPythonScriptSource> executed;
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
    EXPECT_EQ(WebPythonDomError::kLimit, host.SetText(handle, "deny").error);
  });
  Load("<p id=result>before</p><pyscript>bridge</pyscript>");
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

}  // namespace
}  // namespace blink
