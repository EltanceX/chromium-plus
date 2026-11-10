// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "base/strings/string_util.h"
#include "base/time/tick_clock.h"
#include "third_party/blink/renderer/modules/canvas/canvas2d/python_canvas_bridge_impl.h"

namespace blink {
namespace {
using Op = WebPythonCanvasOp;
using Error = WebPythonDomError;
WebPythonDomResult Invalid(const char* message) {
  return {{}, Error::kLimit, WebString::FromUtf8(message)};
}
bool Color(const std::string& s) {
  return (s.size() == 4 || s.size() == 5 || s.size() == 7 || s.size() == 9) &&
         s[0] == '#' &&
         s.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos;
}
struct Bounds {
  double left = 0, top = 0, right = 0, bottom = 0;
  bool empty = true;
  void Add(double x, double y) {
    if (empty) {
      left = right = x;
      top = bottom = y;
      empty = false;
    } else {
      left = std::min(left, x);
      top = std::min(top, y);
      right = std::max(right, x);
      bottom = std::max(bottom, y);
    }
  }
  uint64_t Pixels(int w, int h, double pad = 2) const {
    if (empty) {
      return 0;
    }
    double x = std::max(
        0.0, std::min(double(w), right + pad) - std::max(0.0, left - pad));
    double y = std::max(
        0.0, std::min(double(h), bottom + pad) - std::max(0.0, top - pad));
    return static_cast<uint64_t>(std::ceil(x) * std::ceil(y));
  }
};
struct DrawingState {
  std::array<double, 6> matrix{1, 0, 0, 1, 0, 0};
  double line_width = 1;
  void Add(Bounds& b, double x, double y) const {
    b.Add(matrix[0] * x + matrix[2] * y + matrix[4],
          matrix[1] * x + matrix[3] * y + matrix[5]);
  }
  Bounds Rect(double x, double y, double w, double h) const {
    Bounds b;
    Add(b, x, y);
    Add(b, x + w, y);
    Add(b, x, y + h);
    Add(b, x + w, y + h);
    return b;
  }
  double StrokePad() const {
    return 2 + 10 * line_width *
                   std::max(std::abs(matrix[0]) + std::abs(matrix[2]),
                            std::abs(matrix[1]) + std::abs(matrix[3]));
  }
};
void AddCost(PythonCanvasCost& a,
             const PythonCanvasCost& b,
             bool subtract = false) {
  if (subtract) {
    a.statistics.commands -= b.statistics.commands;
    a.statistics.bytes -= b.statistics.bytes;
    a.statistics.pixels -= b.statistics.pixels;
    a.work -= b.work;
  } else {
    a.statistics.commands += b.statistics.commands;
    a.statistics.bytes += b.statistics.bytes;
    a.statistics.pixels += b.statistics.pixels;
    a.work += b.work;
  }
}
bool Fits(const PythonCanvasCost& a,
          const PythonCanvasCost& b,
          uint64_t commands,
          uint64_t bytes,
          uint64_t pixels,
          uint64_t work) {
  return b.statistics.commands <=
             commands - std::min(commands, a.statistics.commands) &&
         b.statistics.bytes <= bytes - std::min(bytes, a.statistics.bytes) &&
         b.statistics.pixels <=
             pixels - std::min(pixels, a.statistics.pixels) &&
         b.work <= work - std::min(work, a.work);
}
}  // namespace

WebPythonDomResult ValidatePythonCanvasBatch(
    const std::vector<WebPythonCanvasCommand>& commands,
    int width,
    int height,
    PythonCanvasCost& cost) {
  cost = {};
  if (width < 1 || height < 1 || width > 2048 || height > 2048 ||
      commands.size() > 4096) {
    return Invalid("Canvas batch dimension or command limit exceeded");
  }
  cost.statistics.commands = cost.work = commands.size();
  // Every accepted submission replaces the complete bitmap and drawing state.
  cost.statistics.pixels = uint64_t(width) * height;
  DrawingState state;
  std::vector<DrawingState> stack;
  Bounds path;
  size_t path_points = 0, total_points = 0, text_bytes = 0;
  std::array<unsigned, 33> stops{};
  unsigned gradients = 0;
  for (const auto& c : commands) {
    cost.statistics.bytes += 128 + c.text.size() + c.values.size() * 8;
    if (cost.statistics.bytes > 512 * 1024 || c.values.size() > 16 ||
        c.text.size() > 1024 || !base::IsStringUTF8(c.text)) {
      return Invalid("Canvas batch byte, string or dash limit exceeded");
    }
    for (double value : c.args) {
      if (!std::isfinite(value) || std::abs(value) > 1000000) {
        return Invalid("Canvas numbers must be finite and bounded");
      }
    }
    const auto& a = c.args;
    size_t points = 0;
    switch (c.op) {
      case Op::kClearRect:
      case Op::kFillRect:
      case Op::kStrokeRect:
        cost.statistics.pixels +=
            state.Rect(a[0], a[1], a[2], a[3])
                .Pixels(width, height,
                        c.op == Op::kStrokeRect ? state.StrokePad() : 2);
        break;
      case Op::kBeginPath:
        path = {};
        path_points = 0;
        break;
      case Op::kClosePath:
        points = 1;
        break;
      case Op::kMoveTo:
      case Op::kLineTo:
        state.Add(path, a[0], a[1]);
        points = 1;
        break;
      case Op::kArc:
      case Op::kEllipse: {
        if (a[2] < 0 || (c.op == Op::kEllipse && a[3] < 0) ||
            (c.op == Op::kArc ? (a[5] != 0 && a[5] != 1)
                              : (a[7] != 0 && a[7] != 1))) {
          return Invalid("Invalid Canvas radius or direction");
        }
        const double r = c.op == Op::kArc ? a[2] : a[2] + a[3];
        for (int x : {-1, 1}) {
          for (int y : {-1, 1}) {
            state.Add(path, a[0] + x * r, a[1] + y * r);
          }
        }
        points = 64;
        break;
      }
      case Op::kFill:
      case Op::kStroke:
        cost.statistics.pixels += path.Pixels(
            width, height, c.op == Op::kStroke ? state.StrokePad() : 2);
        cost.work += path_points;
        break;
      case Op::kSave:
        if (stack.size() == 32) {
          return Invalid("Canvas state stack exceeds 32 saves");
        }
        stack.push_back(state);
        break;
      case Op::kRestore:
        if (stack.empty()) {
          return Invalid("Canvas restore has no matching save");
        }
        state = stack.back();
        stack.pop_back();
        break;
      case Op::kTranslate:
        state.matrix[4] += state.matrix[0] * a[0] + state.matrix[2] * a[1];
        state.matrix[5] += state.matrix[1] * a[0] + state.matrix[3] * a[1];
        break;
      case Op::kScale:
        state.matrix[0] *= a[0];
        state.matrix[1] *= a[0];
        state.matrix[2] *= a[1];
        state.matrix[3] *= a[1];
        break;
      case Op::kRotate: {
        const auto old = state.matrix;
        const double co = std::cos(a[0]), si = std::sin(a[0]);
        state.matrix[0] = old[0] * co + old[2] * si;
        state.matrix[1] = old[1] * co + old[3] * si;
        state.matrix[2] = old[2] * co - old[0] * si;
        state.matrix[3] = old[3] * co - old[1] * si;
        break;
      }
      case Op::kFillColor:
      case Op::kStrokeColor:
        if (!Color(c.text)) {
          return Invalid("Canvas requires a hexadecimal color");
        }
        break;
      case Op::kLinearGradient:
        if (gradients == 32 || c.index != ++gradients) {
          return Invalid("Invalid Canvas gradient index or count");
        }
        break;
      case Op::kColorStop:
        if (!c.index || c.index > gradients || ++stops[c.index] > 16 ||
            a[0] < 0 || a[0] > 1 || !Color(c.text)) {
          return Invalid("Invalid Canvas gradient stop");
        }
        break;
      case Op::kFillGradient:
      case Op::kStrokeGradient:
        if (!c.index || c.index > gradients) {
          return Invalid("Expired or foreign batch gradient");
        }
        break;
      case Op::kLineWidth:
        if (a[0] <= 0 || a[0] > 128) {
          return Invalid("Canvas line width must be in (0,128]");
        }
        state.line_width = a[0];
        break;
      case Op::kLineDash: {
        double sum = 0;
        for (double n : c.values) {
          if (!std::isfinite(n) || n < 0) {
            return Invalid("Invalid Canvas dash number");
          }
          sum += n;
        }
        if (sum > 4096) {
          return Invalid("Canvas dash sum exceeds 4096");
        }
        break;
      }
      case Op::kAlpha:
        if (a[0] < 0 || a[0] > 1) {
          return Invalid("Canvas alpha must be in [0,1]");
        }
        break;
      case Op::kFont:
        if (a[0] < 1 || a[0] > 128 || (a[1] != 0 && a[1] != 1) ||
            (c.text != "monospace" && c.text != "sans-serif" &&
             c.text != "serif")) {
          return Invalid(
              "Canvas requires size [1,128] and a generic font family");
        }
        break;
      case Op::kTextAlign:
        if (c.text != "left" && c.text != "right" && c.text != "center" &&
            c.text != "start" && c.text != "end") {
          return Invalid("Invalid Canvas text alignment");
        }
        break;
      case Op::kTextBaseline:
        if (c.text != "top" && c.text != "hanging" && c.text != "middle" &&
            c.text != "alphabetic" && c.text != "ideographic" &&
            c.text != "bottom") {
          return Invalid("Invalid Canvas text baseline");
        }
        break;
      case Op::kFillText:
        text_bytes += c.text.size();
        cost.work += c.text.size();
        cost.statistics.pixels += uint64_t(width) * height;
        if (text_bytes > 16384) {
          return Invalid("Canvas text exceeds 16 KiB per batch");
        }
        break;
      case Op::kSmoothing:
        if (a[0] != 0 && a[0] != 1) {
          return Invalid("Invalid Canvas smoothing boolean");
        }
        break;
      case Op::kDrawSurface:
        if (!c.source.document || !c.source.generation || !c.source.id ||
            !c.source.version || a[2] <= 0 || a[3] <= 0) {
          return Invalid("Invalid Canvas source surface or destination");
        }
        cost.statistics.pixels +=
            state.Rect(a[0], a[1], a[2], a[3]).Pixels(width, height);
        break;
      default:
        return Invalid("Unknown Canvas command");
    }
    path_points += points;
    total_points += points;
    cost.work += points;
    if (path_points > 2048 || total_points > 8192) {
      return Invalid("Canvas path point limit exceeded");
    }
    for (double m : state.matrix) {
      if (!std::isfinite(m) || std::abs(m) > 1000000) {
        return Invalid("Canvas transform exceeds its bound");
      }
    }
  }
  if (!stack.empty()) {
    return Invalid("Canvas batch must balance save and restore");
  }
  return {};
}

PythonCanvasDrawingBudget::PythonCanvasDrawingBudget(
    const base::TickClock* clock)
    : clock_(*clock) {}
void PythonCanvasDrawingBudget::BeginExecution() {
  execution_ = {};
}
WebPythonDomResult PythonCanvasDrawingBudget::Consume(
    const PythonCanvasCost& cost) {
  const auto now = clock_->NowTicks();
  while (!usages_.empty() && now - usages_.front().time >= base::Seconds(1)) {
    AddCost(window_, usages_.front().cost, true);
    usages_.pop_front();
  }
  if (stopped_ ||
      !Fits(execution_, cost, 8192, 2 * 1024 * 1024, 32 * 1024 * 1024, 32768)) {
    stopped_ = true;
    return {
        {}, Error::kDrawingLimit, "Python exceeded the Canvas drawing budget"};
  }
  if (!Fits(window_, cost, 131072, 16 * 1024 * 1024, 512 * 1024 * 1024,
            1048576)) {
    return {{},
            Error::kState,
            "Canvas drawing window is full; batch was not drawn"};
  }
  AddCost(execution_, cost);
  AddCost(window_, cost);
  if (usages_.size() == 1024) {
    usages_.back().time = now;
    AddCost(usages_.back().cost, cost);
  } else {
    usages_.push_back(Usage{now, cost});
  }
  return {};
}
}  // namespace blink
