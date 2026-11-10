// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_PYTHON_CANVAS_TYPES_H_
#define THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_PYTHON_CANVAS_TYPES_H_

#include <stdint.h>

#include <array>
#include <string>
#include <vector>

namespace blink {

enum class WebPythonCanvasOp {
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

struct WebPythonCanvasHandle {
  uint64_t document = 0;
  uint64_t generation = 0;
  uint64_t id = 0;
  uint64_t version = 0;
};

// UTF-8 and numeric payloads only; no Python, V8 or DOM object ownership.
struct WebPythonCanvasCommand {
  WebPythonCanvasOp op = WebPythonCanvasOp::kClearRect;
  std::array<double, 8> args{};
  std::string text;
  std::vector<double> values;
  WebPythonCanvasHandle source;
  uint32_t index = 0;
};

struct WebPythonCanvasStatistics {
  uint64_t commands = 0;
  uint64_t bytes = 0;
  uint64_t pixels = 0;
  double native_ms = 0;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_PYTHON_CANVAS_TYPES_H_
