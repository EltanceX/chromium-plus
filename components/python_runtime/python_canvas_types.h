// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PYTHON_RUNTIME_PYTHON_CANVAS_TYPES_H_
#define COMPONENTS_PYTHON_RUNTIME_PYTHON_CANVAS_TYPES_H_

#include <stdint.h>

#include <array>
#include <string>
#include <vector>

namespace native_python {

enum class CanvasOp {
  kClearRect,
  kFillRect,
  kStrokeRect,
  kBeginPath,
  kClosePath,
  kMoveTo,
  kLineTo,
  kArc,
  kEllipse,
  kFill,
  kStroke,
  kSave,
  kRestore,
  kTranslate,
  kScale,
  kRotate,
  kFillColor,
  kStrokeColor,
  kFillGradient,
  kStrokeGradient,
  kLinearGradient,
  kColorStop,
  kLineWidth,
  kLineDash,
  kAlpha,
  kFont,
  kTextAlign,
  kTextBaseline,
  kFillText,
  kSmoothing,
  kDrawSurface,
};

struct CanvasHandle {
  uint64_t document = 0;
  uint64_t generation = 0;
  uint64_t id = 0;
  uint64_t version = 0;
};

// UTF-8 and numeric payloads only; no Python, V8 or DOM object ownership.
struct CanvasCommand {
  CanvasOp op = CanvasOp::kClearRect;
  std::array<double, 8> args{};
  std::string text;
  std::vector<double> values;
  CanvasHandle source;
  uint32_t index = 0;
};

struct CanvasStatistics {
  uint64_t commands = 0;
  uint64_t bytes = 0;
  uint64_t pixels = 0;
  double native_ms = 0;
};

}  // namespace native_python

#endif  // COMPONENTS_PYTHON_RUNTIME_PYTHON_CANVAS_TYPES_H_
