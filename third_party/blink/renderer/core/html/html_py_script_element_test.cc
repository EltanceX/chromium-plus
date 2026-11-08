// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/html_py_script_element.h"

#include "base/test/bind.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/web/web_python_script_runner.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/html/html_unknown_element.h"
#include "third_party/blink/renderer/core/testing/sim/sim_request.h"
#include "third_party/blink/renderer/core/testing/sim/sim_test.h"
#include "third_party/blink/renderer/platform/testing/runtime_enabled_features_test_helpers.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"

namespace blink {
namespace {

class RecordingPythonRunner final : public WebPythonScriptRunner {
 public:
  bool IsOriginAllowed(const WebSecurityOrigin& origin) const override {
    return allow && origin.ToString() == "https://example.test";
  }
  std::vector<WebPythonScriptResult> Run(
      const std::vector<WebPythonScriptSource>& sources) override {
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

}  // namespace
}  // namespace blink
