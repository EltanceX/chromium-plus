// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <limits>

#include "base/test/simple_test_tick_clock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/html/canvas/html_canvas_element.h"
#include "third_party/blink/renderer/core/testing/page_test_base.h"
#include "third_party/blink/renderer/modules/canvas/canvas2d/canvas_rendering_context_2d.h"
#include "third_party/blink/renderer/modules/canvas/canvas2d/python_canvas_bridge_impl.h"

namespace blink {
namespace {
using Op = WebPythonCanvasOp;
using Error = WebPythonDomError;
WebPythonCanvasCommand Command(Op op,
                               std::initializer_list<double> args = {},
                               std::string text = {}) {
  WebPythonCanvasCommand c;
  c.op = op;
  std::copy(args.begin(), args.end(), c.args.begin());
  c.text = std::move(text);
  return c;
}
Error Validate(std::vector<WebPythonCanvasCommand> commands) {
  PythonCanvasCost cost;
  return ValidatePythonCanvasBatch(commands, 320, 200, cost).error;
}

class PythonCanvasBridgeTest : public PageTestBase {
 protected:
  void SetUp() override {
    PageTestBase::SetUp();
    SetBodyInnerHTML("<canvas id='game' width='32' height='32'></canvas>");
    bridge_ =
        MakeGarbageCollected<PythonCanvasBridgeImpl>(GetDocument(), 71, 9);
  }
  void TearDown() override {
    bridge_->Dispose();
    bridge_ = nullptr;
    PageTestBase::TearDown();
  }
  HTMLCanvasElement& Canvas() {
    return *To<HTMLCanvasElement>(GetElementById("game"));
  }
  WebPythonCanvasResult Submit(
      WebPythonCanvasHandle h,
      std::vector<WebPythonCanvasCommand> commands = {}) {
    return bridge_->Submit(h, commands, [] { return true; });
  }
  Persistent<PythonCanvasBridgeImpl> bridge_;
};

TEST_F(PythonCanvasBridgeTest, IdentityVersionAndDisposeRevokeHandles) {
  auto h = bridge_->GetContext(Canvas()).handle;
  ASSERT_NE(0u, h.id);
  EXPECT_EQ(h.version, bridge_->GetContext(Canvas()).handle.version);
  auto foreign = h;
  ++foreign.document;
  EXPECT_EQ(Error::kReference, Submit(foreign).status.error);
  foreign = h;
  ++foreign.generation;
  EXPECT_EQ(Error::kReference, Submit(foreign).status.error);
  auto resized = bridge_->Resize(h, 16, 16);
  EXPECT_EQ(Error::kNone, resized.status.error);
  EXPECT_NE(h.version, resized.handle.version);
  EXPECT_EQ(Error::kReference, Submit(h).status.error);
  EXPECT_EQ(Error::kNone, Submit(resized.handle).status.error);
  bridge_->Dispose();
  EXPECT_EQ(Error::kReference, Submit(resized.handle).status.error);
  EXPECT_NE(Error::kNone, bridge_->CreateSurface(1, 1).status.error);
}

TEST_F(PythonCanvasBridgeTest, InvalidBatchDoesNotResetNativeContextState) {
  auto h = bridge_->GetContext(Canvas()).handle;
  auto& ctx =
      *static_cast<CanvasRenderingContext2D*>(Canvas().RenderingContext());
  ctx.setGlobalAlpha(0.25);
  EXPECT_EQ(Error::kLimit,
            Submit(h, {Command(Op::kAlpha, {0.5}), Command(Op::kRestore)})
                .status.error);
  EXPECT_EQ(0.25, ctx.globalAlpha());
  EXPECT_EQ(Error::kNone, Submit(h).status.error);
  EXPECT_EQ(1, ctx.globalAlpha());
}

TEST_F(PythonCanvasBridgeTest, DetachedCanvasAndOldResourceSlotExpire) {
  auto h = bridge_->GetContext(Canvas()).handle;
  Canvas().remove();
  EXPECT_EQ(Error::kReference, Submit(h).status.error);
  auto surface = bridge_->CreateSurface(16, 16).handle;
  EXPECT_EQ(h.id, surface.id);
  EXPECT_NE(h.version, surface.version);
  EXPECT_EQ(Error::kReference, Submit(h).status.error);
  EXPECT_EQ(Error::kNone, Submit(surface).status.error);
  EXPECT_EQ(Error::kNone, bridge_->Release(surface).error);
  EXPECT_EQ(Error::kReference, Submit(surface).status.error);
}

TEST_F(PythonCanvasBridgeTest, InactiveSubmissionDoesNotResetNativeState) {
  auto h = bridge_->GetContext(Canvas()).handle;
  auto& ctx =
      *static_cast<CanvasRenderingContext2D*>(Canvas().RenderingContext());
  ctx.setGlobalAlpha(0.25);
  EXPECT_EQ(Error::kState,
            bridge_->Submit(h, {}, [] { return false; }).status.error);
  EXPECT_EQ(0.25, ctx.globalAlpha());
}

TEST(PythonCanvasBatchTest, ChargesResetRectanglesTransformsAndUtf8Text) {
  PythonCanvasCost cost;
  const auto result = ValidatePythonCanvasBatch(
      {Command(Op::kTranslate, {10, 20}), Command(Op::kScale, {2, 3}),
       Command(Op::kFillRect, {0, 0, 5, 4}),
       Command(Op::kFillText, {0, 0}, "hello")},
      320, 200, cost);
  EXPECT_EQ(Error::kNone, result.error);
  EXPECT_EQ(4u, cost.statistics.commands);
  EXPECT_EQ(4 * 128u + 5, cost.statistics.bytes);
  EXPECT_EQ(2 * 64000u + 14 * 16, cost.statistics.pixels);
  EXPECT_EQ(9u, cost.work);
}
TEST(PythonCanvasBatchTest, RejectsUnknownCommandsAndNonFinitePayloads) {
  EXPECT_EQ(Error::kLimit, Validate({Command(static_cast<Op>(1000))}));
  EXPECT_EQ(
      Error::kLimit,
      Validate({Command(Op::kFillRect,
                        {0, 0, 1, std::numeric_limits<double>::infinity()})}));
  EXPECT_EQ(
      Error::kLimit,
      Validate({Command(Op::kFillRect,
                        {std::numeric_limits<double>::quiet_NaN(), 0, 1, 1})}));
  EXPECT_EQ(Error::kLimit,
            Validate({Command(Op::kFillText, {0, 0}, std::string("\xff"))}));
}
TEST(PythonCanvasBatchTest, RejectsInvalidColorsAndOutOfRangeStyles) {
  for (auto c :
       {Command(Op::kFillColor, {}, "red"),
        Command(Op::kStrokeColor, {}, "#xyz"), Command(Op::kAlpha, {1.1}),
        Command(Op::kLineWidth, {0}), Command(Op::kSmoothing, {2}),
        Command(Op::kTextAlign, {}, "random"),
        Command(Op::kTextBaseline, {}, "random"),
        Command(Op::kFont, {129, 0}, "serif"),
        Command(Op::kFont, {12, 0}, "url(x)")}) {
    EXPECT_EQ(Error::kLimit, Validate({c}));
  }
  EXPECT_EQ(Error::kNone,
            Validate({Command(Op::kFillColor, {}, "#1234"),
                      Command(Op::kStrokeColor, {}, "#12345678")}));
}
TEST(PythonCanvasBatchTest, StateStackMustBalanceAndNeverExceed32) {
  std::vector<WebPythonCanvasCommand> commands(32, Command(Op::kSave));
  commands.insert(commands.end(), 32, Command(Op::kRestore));
  EXPECT_EQ(Error::kNone, Validate(commands));
  commands.insert(commands.begin(), Command(Op::kSave));
  EXPECT_EQ(Error::kLimit, Validate(commands));
  EXPECT_EQ(Error::kLimit, Validate({Command(Op::kRestore)}));
  EXPECT_EQ(Error::kLimit, Validate({Command(Op::kSave)}));
}
TEST(PythonCanvasBatchTest, PathBoundsAndRepeatedFillsChargeActualWork) {
  PythonCanvasCost cost;
  std::vector<WebPythonCanvasCommand> commands(2048,
                                               Command(Op::kLineTo, {10, 10}));
  commands.push_back(Command(Op::kFill));
  commands.push_back(Command(Op::kStroke));
  EXPECT_EQ(Error::kNone,
            ValidatePythonCanvasBatch(commands, 320, 200, cost).error);
  EXPECT_EQ(2050u + 2048u + 4096u, cost.work);
  commands.insert(commands.begin(), Command(Op::kMoveTo));
  EXPECT_EQ(Error::kLimit, Validate(commands));
}
TEST(PythonCanvasBatchTest, ArcEllipseDirectionsAndTransformsAreBounded) {
  EXPECT_EQ(Error::kNone,
            Validate({Command(Op::kArc, {10, 10, 5, 0, 6, 1}),
                      Command(Op::kEllipse, {10, 10, 5, 3, 0, 0, 6, 0}),
                      Command(Op::kFill)}));
  EXPECT_EQ(Error::kLimit, Validate({Command(Op::kArc, {0, 0, -1, 0, 1})}));
  EXPECT_EQ(Error::kLimit, Validate({Command(Op::kEllipse, {0, 0, 1, -1})}));
  EXPECT_EQ(Error::kLimit, Validate({Command(Op::kArc, {0, 0, 1, 0, 1, 2})}));
  EXPECT_EQ(Error::kLimit, Validate({Command(Op::kScale, {1000000, 1000000}),
                                     Command(Op::kScale, {1000000, 1000000})}));
}
TEST(PythonCanvasBatchTest, GradientsAreLocalOrderedAndHave16Stops) {
  auto gradient = Command(Op::kLinearGradient, {0, 0, 1, 1});
  gradient.index = 1;
  auto stop = Command(Op::kColorStop, {0.5}, "#fff");
  stop.index = 1;
  auto fill = Command(Op::kFillGradient);
  fill.index = 1;
  std::vector<WebPythonCanvasCommand> commands{gradient};
  commands.insert(commands.end(), 16, stop);
  commands.push_back(fill);
  EXPECT_EQ(Error::kNone, Validate(commands));
  commands.push_back(stop);
  EXPECT_EQ(Error::kLimit, Validate(commands));
  EXPECT_EQ(Error::kLimit, Validate({fill}));
  EXPECT_EQ(Error::kLimit, Validate({stop, gradient}));
  std::vector<WebPythonCanvasCommand> many;
  for (unsigned i = 1; i <= 32; ++i) {
    gradient.index = i;
    many.push_back(gradient);
  }
  EXPECT_EQ(Error::kNone, Validate(many));
  gradient.index = 33;
  many.push_back(gradient);
  EXPECT_EQ(Error::kLimit, Validate(many));
}
TEST(PythonCanvasBatchTest, CommandAndByteBoundsAreIndependent) {
  std::vector<WebPythonCanvasCommand> commands(4096, Command(Op::kBeginPath));
  EXPECT_EQ(Error::kNone, Validate(commands));
  commands.push_back(Command(Op::kBeginPath));
  EXPECT_EQ(Error::kLimit, Validate(commands));
  commands.resize(4096);
  commands.back() = Command(Op::kFillColor, {}, "#fff");
  EXPECT_EQ(Error::kLimit, Validate(commands));
}
TEST(PythonCanvasBatchTest, TextAndDashPayloadsHaveTheirOwnLimits) {
  std::vector<WebPythonCanvasCommand> commands(
      16, Command(Op::kFillText, {0, 0}, std::string(1024, 'x')));
  EXPECT_EQ(Error::kNone, Validate(commands));
  commands.push_back(Command(Op::kFillText, {0, 0}, "x"));
  EXPECT_EQ(Error::kLimit, Validate(commands));
  auto dash = Command(Op::kLineDash);
  dash.values.assign(16, 1);
  EXPECT_EQ(Error::kNone, Validate({dash}));
  dash.values.push_back(1);
  EXPECT_EQ(Error::kLimit, Validate({dash}));
  dash.values = {-1};
  EXPECT_EQ(Error::kLimit, Validate({dash}));
  dash.values = {std::numeric_limits<double>::quiet_NaN()};
  EXPECT_EQ(Error::kLimit, Validate({dash}));
}
TEST(PythonCanvasBatchTest, SurfaceHandlesAndDimensionsMustBeValid) {
  auto c = Command(Op::kDrawSurface, {0, 0, 10, 10});
  EXPECT_EQ(Error::kLimit, Validate({c}));
  c.source = {7, 1, 1, 1};
  EXPECT_EQ(Error::kNone, Validate({c}));
  PythonCanvasCost cost;
  EXPECT_EQ(Error::kLimit, ValidatePythonCanvasBatch({}, 2049, 1, cost).error);
  EXPECT_EQ(Error::kNone,
            ValidatePythonCanvasBatch({}, 2048, 2048, cost).error);
  EXPECT_EQ(2048u * 2048u, cost.statistics.pixels);
}
TEST(PythonCanvasBatchTest,
     ExecutionLimitIsFatalAndSurvivesBeginAndTimeExpiry) {
  base::SimpleTestTickClock clock;
  PythonCanvasDrawingBudget budget(&clock);
  PythonCanvasCost cost;
  cost.statistics.pixels = 16 * 1024 * 1024;
  EXPECT_EQ(Error::kNone, budget.Consume(cost).error);
  EXPECT_EQ(Error::kNone, budget.Consume(cost).error);
  EXPECT_EQ(Error::kDrawingLimit, budget.Consume(cost).error);
  budget.BeginExecution();
  clock.Advance(base::Seconds(2));
  EXPECT_EQ(Error::kDrawingLimit, budget.Consume({}).error);
}
TEST(PythonCanvasBatchTest, SlidingWindowRejectsWithoutChargingAndRecovers) {
  base::SimpleTestTickClock clock;
  PythonCanvasDrawingBudget budget(&clock);
  PythonCanvasCost cost;
  cost.statistics.pixels = 16 * 1024 * 1024;
  for (int i = 0; i < 32; ++i) {
    budget.BeginExecution();
    ASSERT_EQ(Error::kNone, budget.Consume(cost).error);
  }
  budget.BeginExecution();
  EXPECT_EQ(Error::kState, budget.Consume(cost).error);
  budget.BeginExecution();
  clock.Advance(base::Milliseconds(999));
  EXPECT_EQ(Error::kState, budget.Consume(cost).error);
  clock.Advance(base::Milliseconds(1));
  EXPECT_EQ(Error::kNone, budget.Consume(cost).error);
}
TEST(PythonCanvasBatchTest,
     ExecutionCommandDataAndPathWorkLimitsAreIndependent) {
  for (int field = 0; field < 3; ++field) {
    base::SimpleTestTickClock clock;
    PythonCanvasDrawingBudget budget(&clock);
    PythonCanvasCost cost;
    if (field == 0) {
      cost.statistics.commands = 8193;
    }
    if (field == 1) {
      cost.statistics.bytes = 2 * 1024 * 1024 + 1;
    }
    if (field == 2) {
      cost.work = 32769;
    }
    EXPECT_EQ(Error::kDrawingLimit, budget.Consume(cost).error);
  }
}

TEST(PythonCanvasBatchTest, WindowCommandDataAndWorkLimitsAreIndependent) {
  for (int field = 0; field < 3; ++field) {
    base::SimpleTestTickClock clock;
    PythonCanvasDrawingBudget budget(&clock);
    PythonCanvasCost cost;
    const int count = field == 1 ? 16 : 32;
    if (field == 0) {
      cost.statistics.commands = 4096;
    } else if (field == 1) {
      cost.statistics.bytes = 1024 * 1024;
    } else {
      cost.work = 32768;
    }
    for (int i = 0; i < count; ++i) {
      budget.BeginExecution();
      ASSERT_EQ(Error::kNone, budget.Consume(cost).error);
    }
    budget.BeginExecution();
    EXPECT_EQ(Error::kState, budget.Consume(cost).error);
    clock.Advance(base::Seconds(1));
    EXPECT_EQ(Error::kNone, budget.Consume(cost).error);
  }
}

TEST(PythonCanvasBatchTest, FullLedgerMergesConservativelyAndExpiresSafely) {
  base::SimpleTestTickClock clock;
  PythonCanvasDrawingBudget budget(&clock);
  PythonCanvasCost cost;
  cost.statistics.commands = 1;
  for (int i = 0; i < 1024; ++i) {
    ASSERT_EQ(Error::kNone, budget.Consume(cost).error);
  }
  clock.Advance(base::Milliseconds(500));
  cost.statistics.commands = 8192;
  for (int i = 0; i < 15; ++i) {
    budget.BeginExecution();
    ASSERT_EQ(Error::kNone, budget.Consume(cost).error);
  }
  cost.statistics.commands = 7168;
  budget.BeginExecution();
  ASSERT_EQ(Error::kNone, budget.Consume(cost).error);
  clock.Advance(base::Milliseconds(500));
  budget.BeginExecution();
  cost.statistics.commands = 1024;
  EXPECT_EQ(Error::kState, budget.Consume(cost).error);
  cost.statistics.commands = 1023;
  EXPECT_EQ(Error::kNone, budget.Consume(cost).error);
  clock.Advance(base::Milliseconds(499));
  budget.BeginExecution();
  cost.statistics.commands = 8192;
  EXPECT_EQ(Error::kState, budget.Consume(cost).error);
  clock.Advance(base::Milliseconds(1));
  EXPECT_EQ(Error::kNone, budget.Consume(cost).error);
}
}  // namespace
}  // namespace blink
