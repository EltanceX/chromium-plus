// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <limits>
#include <string>

#include "base/functional/bind.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/document_type.h"
#include "third_party/blink/renderer/core/dom/dom_implementation.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/dom/processing_instruction.h"
#include "third_party/blink/renderer/core/dom/shadow_root.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/testing/page_test_base.h"
#include "third_party/blink/renderer/modules/python_browser_bridge_impl.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"

namespace blink {
namespace {
using Op = WebPythonBrowserOp;
using Error = WebPythonDomError;
class PythonBrowserBridgeTest : public PageTestBase {
 public:
  PythonBrowserBridgeTest()
      : PageTestBase(base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}

  PythonBrowserBridgeImpl* Bridge() {
    return MakeGarbageCollected<PythonBrowserBridgeImpl>(GetDocument());
  }
};
TEST_F(PythonBrowserBridgeTest, AudioAndDownloadRequireUserActivation) {
  auto* bridge = Bridge();
  EXPECT_FALSE(LocalFrame::HasTransientUserActivation(&GetFrame()));
  EXPECT_EQ(Error::kNotAllowed,
            bridge->Run({Op::kAudioResume}, nullptr).status.error);
  EXPECT_EQ(Error::kNotAllowed,
            bridge->Run({Op::kExportDocument, {}, "game.html"}, nullptr)
                .status.error);
  EXPECT_EQ("closed", bridge->Run({Op::kAudioState}, nullptr).value.Utf8());
  bridge->Dispose();
}
TEST_F(PythonBrowserBridgeTest, NativeToneValidationPrecedesAudioScheduling) {
  auto* bridge = Bridge();
  WebPythonBrowserRequest r{Op::kPlayTones};
  EXPECT_EQ(Error::kLimit, bridge->Run(r, nullptr).status.error);
  r.tones.resize(17);
  EXPECT_EQ(Error::kLimit, bridge->Run(r, nullptr).status.error);
  r.tones.resize(1);
  r.tones[0].wave = "sine";
  r.tones[0].frequency = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(Error::kLimit, bridge->Run(r, nullptr).status.error);
  r.tones[0].frequency = 440;
  EXPECT_EQ(Error::kState, bridge->Run(r, nullptr).status.error);
  bridge->Dispose();
}
TEST_F(PythonBrowserBridgeTest, ExportRejectsPathsAndNonHtmlNames) {
  auto* bridge = Bridge();
  for (const char* name : {"../game.html", "C:\\game.html", "game.exe", ""}) {
    WebPythonBrowserRequest r{Op::kExportDocument};
    r.key = WebString::FromUtf8(name);
    EXPECT_EQ(Error::kLimit, bridge->Run(r, nullptr).status.error);
  }
  bridge->Dispose();
}
TEST_F(PythonBrowserBridgeTest, ExportRejectsAuthorShadowAndTemplateContent) {
  LocalFrame::NotifyUserActivation(
      &GetFrame(), mojom::blink::UserActivationNotificationType::kTest);
  ASSERT_TRUE(LocalFrame::HasTransientUserActivation(&GetFrame()));
  auto* bridge = Bridge();
  for (auto mode : {ShadowRootMode::kOpen, ShadowRootMode::kClosed}) {
    SetBodyContent("<div id='host'></div>");
    GetElementById("host")->AttachShadowRootForTesting(mode);
    const auto result =
        bridge->Run({Op::kExportDocument, {}, "game.html"}, nullptr);
    EXPECT_EQ(Error::kNotAllowed, result.status.error);
    EXPECT_EQ("Template and shadow content export is unsupported",
              result.status.message.Utf8());
  }
  SetBodyContent("<template><p>content</p></template>");
  const auto result =
      bridge->Run({Op::kExportDocument, {}, "game.html"}, nullptr);
  EXPECT_EQ(Error::kNotAllowed, result.status.error);
  EXPECT_EQ("Template and shadow content export is unsupported",
            result.status.message.Utf8());
  bridge->Dispose();
}
TEST_F(PythonBrowserBridgeTest, ExportCountsMetadataBeforeSerializing) {
  LocalFrame::NotifyUserActivation(
      &GetFrame(), mojom::blink::UserActivationNotificationType::kTest);
  ASSERT_TRUE(LocalFrame::HasTransientUserActivation(&GetFrame()));
  auto* bridge = Bridge();
  const String large = String::FromUtf8(std::string(1024 * 1024, 'x'));
  if (GetDocument().doctype()) {
    GetDocument().doctype()->remove();
  }
  for (unsigned field = 0; field < 4; ++field) {
    DummyExceptionStateForTesting exception;
    Node* metadata = nullptr;
    if (field < 3) {
      metadata = GetDocument().implementation().createDocumentType(
          AtomicString(field == 0 ? large : String("html")),
          field == 1 ? large : String(), field == 2 ? large : String(),
          exception);
    } else {
      metadata =
          GetDocument().createProcessingInstruction(large, "", exception);
    }
    ASSERT_FALSE(exception.HadException());
    ASSERT_TRUE(metadata);
    GetDocument().InsertBefore(metadata, GetDocument().documentElement());
    const auto result =
        bridge->Run({Op::kExportDocument, {}, "game.html"}, nullptr);
    EXPECT_EQ(Error::kLimit, result.status.error);
    EXPECT_EQ("Document exceeds the export source limit",
              result.status.message.Utf8());
    metadata->remove();
  }
  bridge->Dispose();
}
TEST_F(PythonBrowserBridgeTest, DisposeIsIdempotentAndInvalidatesAllServices) {
  auto* bridge = Bridge();
  bridge->Dispose();
  bridge->Dispose();
  for (auto op : {Op::kAudioResume, Op::kAudioStop, Op::kPoll, Op::kStorageGet,
                  Op::kRequestFullscreen, Op::kExportDocument}) {
    EXPECT_EQ(Error::kNotAllowed, bridge->Run({op}, nullptr).status.error);
  }
}
TEST_F(PythonBrowserBridgeTest, UnknownCompletionsCannotCreateQueueEntries) {
  auto* bridge = Bridge();
  for (uint64_t id = 1; id <= 20; ++id) {
    bridge->Complete(id, true, "forged");
  }
  EXPECT_EQ(0u, bridge->Run({Op::kPoll}, nullptr).id);
  bridge->Dispose();
}
TEST_F(PythonBrowserBridgeTest, DeferredOperationRechecksAuthorization) {
  bool allowed = true;
  auto* bridge = MakeGarbageCollected<PythonBrowserBridgeImpl>(
      GetDocument(),
      base::BindRepeating([](bool* value) { return *value; }, &allowed));
  EXPECT_NE(0u, bridge->Run({Op::kAudioSuspend}, nullptr).id);
  allowed = false;
  task_environment().RunUntilIdle();
  EXPECT_EQ(Error::kNotAllowed, bridge->Run({Op::kPoll}, nullptr).status.error);
}
TEST_F(PythonBrowserBridgeTest, DisposeCancelsQueuedNativeOperation) {
  auto* bridge = MakeGarbageCollected<PythonBrowserBridgeImpl>(
      GetDocument(), base::BindRepeating([] { return true; }));
  EXPECT_NE(0u, bridge->Run({Op::kAudioSuspend}, nullptr).id);
  bridge->Dispose();
  task_environment().RunUntilIdle();
  EXPECT_EQ(Error::kNotAllowed, bridge->Run({Op::kPoll}, nullptr).status.error);
}
TEST_F(PythonBrowserBridgeTest, DeferredFullscreenRejectsDetachedTarget) {
  SetBodyContent("<div id='target'></div>");
  auto* bridge = MakeGarbageCollected<PythonBrowserBridgeImpl>(
      GetDocument(), base::BindRepeating([] { return true; }));
  auto* target = GetElementById("target");
  const auto id = bridge->Run({Op::kRequestFullscreen}, target).id;
  ASSERT_NE(0u, id);
  target->remove();
  task_environment().RunUntilIdle();
  const auto result = bridge->Run({Op::kPoll}, nullptr);
  EXPECT_EQ(id, result.id);
  EXPECT_FALSE(result.flag);
  EXPECT_EQ("Fullscreen target is inactive", result.value.Utf8());
  EXPECT_EQ(0u, bridge->Run({Op::kPoll}, nullptr).id);
  bridge->Dispose();
}
TEST_F(PythonBrowserBridgeTest, ExpiredQueuedOperationCompletesOnlyOnce) {
  auto* bridge = MakeGarbageCollected<PythonBrowserBridgeImpl>(
      GetDocument(), base::BindRepeating([] { return true; }));
  const auto id = bridge->Run({Op::kAudioSuspend}, nullptr).id;
  ASSERT_NE(0u, id);
  AdvanceClock(base::Seconds(6));
  task_environment().RunUntilIdle();
  const auto result = bridge->Run({Op::kPoll}, nullptr);
  EXPECT_EQ(id, result.id);
  EXPECT_FALSE(result.flag);
  EXPECT_EQ("Browser operation timed out", result.value.Utf8());
  EXPECT_EQ(0u, bridge->Run({Op::kPoll}, nullptr).id);
  EXPECT_EQ("closed", bridge->Run({Op::kAudioState}, nullptr).value.Utf8());
  bridge->Dispose();
}
TEST_F(PythonBrowserBridgeTest, StorageLimitsCountUtf8Bytes) {
  auto* bridge = Bridge();
  WebPythonBrowserRequest request{Op::kStorageSet};
  std::string text;
  for (unsigned i = 0; i < 86; ++i) {
    text += "中";
  }
  request.key = WebString::FromUtf8(text);
  EXPECT_EQ(Error::kLimit, bridge->Run(request, nullptr).status.error);
  request.key = "score";
  while (text.size() <= 16384) {
    text += "中";
  }
  request.value = WebString::FromUtf8(text);
  EXPECT_EQ(Error::kLimit, bridge->Run(request, nullptr).status.error);
  bridge->Dispose();
}
}  // namespace
}  // namespace blink
