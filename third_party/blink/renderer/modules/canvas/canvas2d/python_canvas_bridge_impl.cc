// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/canvas/canvas2d/python_canvas_bridge_impl.h"

#include "base/check.h"
#include "base/time/default_tick_clock.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_canvas_text_align.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_canvas_text_baseline.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/html/canvas/canvas_context_creation_attributes_core.h"
#include "third_party/blink/renderer/core/html/canvas/canvas_image_source.h"
#include "third_party/blink/renderer/core/html/canvas/html_canvas_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/modules/canvas/canvas2d/canvas_gradient.h"
#include "third_party/blink/renderer/modules/canvas/canvas2d/canvas_rendering_context_2d.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/graphics/color.h"
#include "third_party/blink/renderer/platform/wtf/text/strcat.h"

namespace blink {
namespace {
using Error = WebPythonDomError;
using Op = WebPythonCanvasOp;
WebPythonDomResult Expired() {
  return {{}, Error::kReference, "Expired Canvas handle"};
}
WebPythonDomResult Limit() {
  return {
      {},
      Error::kLimit,
      "Canvas allows 8 resources, 2048 per dimension and 8 Mi pixels in total"};
}
Color ParseColor(const std::string& text) {
  Color color;
  CHECK(color.SetFromString(String::FromUtf8(text)));
  return color;
}
}  // namespace

class PythonCanvasEntry final : public GarbageCollected<PythonCanvasEntry> {
 public:
  PythonCanvasEntry(HTMLCanvasElement& c, WebPythonCanvasHandle h, bool own)
      : canvas(&c),
        owned(own ? &c : nullptr),
        handle(h),
        width(c.width()),
        height(c.height()) {}
  void Trace(Visitor* visitor) const {
    visitor->Trace(canvas);
    visitor->Trace(owned);
  }
  WeakMember<HTMLCanvasElement> canvas;
  Member<HTMLCanvasElement> owned;
  WebPythonCanvasHandle handle;
  uint32_t width;
  uint32_t height;
};

PythonCanvasBridge* PythonCanvasBridgeImpl::Create(Document& d,
                                                   uint64_t id,
                                                   uint64_t generation) {
  return MakeGarbageCollected<PythonCanvasBridgeImpl>(d, id, generation);
}
PythonCanvasBridgeImpl::PythonCanvasBridgeImpl(Document& d,
                                               uint64_t id,
                                               uint64_t generation)
    : document_(&d),
      document_id_(id),
      generation_(generation),
      budget_(base::DefaultTickClock::GetInstance()) {}
void PythonCanvasBridgeImpl::Trace(Visitor* visitor) const {
  visitor->Trace(document_);
  visitor->Trace(entries_);
  PythonCanvasBridge::Trace(visitor);
}
void PythonCanvasBridgeImpl::Dispose() {
  disposed_ = true;
  entries_.clear();
}

bool PythonCanvasBridgeImpl::Fits(uint32_t w,
                                  uint32_t h,
                                  const PythonCanvasEntry* except) {
  if (disposed_ || !document_ || w < 1 || h < 1 || w > 2048 || h > 2048) {
    return false;
  }
  uint64_t pixels = uint64_t(w) * h;
  for (auto& entry : entries_) {
    if (!entry) {
      continue;
    }
    auto* c = entry->canvas.Get();
    if (!c || &c->GetDocument() != document_ ||
        (!entry->owned && (!c->isConnected() || c->IsInShadowTree())) ||
        c->width() > 2048 || c->height() > 2048 || !c->width() ||
        !c->height()) {
      entry = nullptr;
      continue;
    }
    if (entry != except) {
      pixels += uint64_t(c->width()) * c->height();
    }
  }
  return pixels <= 8 * 1024 * 1024;
}
PythonCanvasEntry* PythonCanvasBridgeImpl::Resolve(WebPythonCanvasHandle h) {
  if (disposed_ || !document_ || h.document != document_id_ ||
      h.generation != generation_ || !h.id || h.id > entries_.size()) {
    return nullptr;
  }
  auto* e = entries_[static_cast<wtf_size_t>(h.id - 1)].Get();
  if (!e || e->handle.version != h.version) {
    return nullptr;
  }
  auto* c = e->canvas.Get();
  if (!c || &c->GetDocument() != document_ ||
      (!e->owned && (!c->isConnected() || c->IsInShadowTree())) ||
      c->GetCustomElementState() != CustomElementState::kUncustomized ||
      c->FastHasAttribute(html_names::kIsAttr) || !c->RenderingContext() ||
      !c->RenderingContext()->IsRenderingContext2D() || !c->OriginClean()) {
    entries_[static_cast<wtf_size_t>(h.id - 1)] = nullptr;
    return nullptr;
  }
  if (c->width() != e->width || c->height() != e->height) {
    return nullptr;
  }
  return e;
}
WebPythonCanvasResult PythonCanvasBridgeImpl::Acquire(HTMLCanvasElement& canvas,
                                                      bool owned) {
  for (auto& e : entries_) {
    if (!e || e->canvas != &canvas) {
      continue;
    }
    if (Resolve(e->handle)) {
      return {{}, e->handle};
    }
    if (e && Fits(canvas.width(), canvas.height(), e)) {
      e->width = canvas.width();
      e->height = canvas.height();
      e->handle.version = ++version_;
      return {{}, e->handle};
    }
    return {Limit()};
  }
  if (!Fits(canvas.width(), canvas.height())) {
    return {Limit()};
  }
  if (canvas.RenderingContext()) {
    return {
        {{},
         Error::kNotAllowed,
         "Canvas already has a context; Python requires its own 2D canvas"}};
  }
  wtf_size_t slot = 0;
  while (slot < entries_.size() && entries_[slot]) {
    ++slot;
  }
  if (slot == 8) {
    return {Limit()};
  }
  auto* ctx =
      canvas.GetCanvasRenderingContext(document_->GetExecutionContext(), "2d",
                                       CanvasContextCreationAttributesCore());
  if (!ctx || !ctx->IsRenderingContext2D()) {
    return {{{}, Error::kState, "Unable to create a native 2D context"}};
  }
  WebPythonCanvasHandle h{document_id_, generation_, slot + 1, ++version_};
  auto* entry = MakeGarbageCollected<PythonCanvasEntry>(canvas, h, owned);
  if (slot == entries_.size()) {
    entries_.push_back(entry);
  } else {
    entries_[slot] = entry;
  }
  return {{}, h};
}
WebPythonCanvasResult PythonCanvasBridgeImpl::GetContext(
    HTMLCanvasElement& canvas) {
  return Acquire(canvas, false);
}
WebPythonCanvasResult PythonCanvasBridgeImpl::CreateSurface(uint32_t w,
                                                            uint32_t h) {
  if (!Fits(w, h)) {
    return {Limit()};
  }
  auto* canvas = MakeGarbageCollected<HTMLCanvasElement>(*document_);
  canvas->SetSize(gfx::Size(w, h));
  return Acquire(*canvas, true);
}
WebPythonCanvasResult PythonCanvasBridgeImpl::Resize(WebPythonCanvasHandle h,
                                                     uint32_t w,
                                                     uint32_t height) {
  auto* e = Resolve(h);
  if (!e) {
    return {Expired()};
  }
  if (!Fits(w, height, e)) {
    return {Limit()};
  }
  PythonCanvasCost cost;
  cost.statistics.commands = cost.work = 1;
  cost.statistics.pixels =
      uint64_t(w) * height + uint64_t(e->width) * e->height;
  auto status = budget_.Consume(cost);
  if (status.error != Error::kNone) {
    return {status};
  }
  e->canvas->SetSize(gfx::Size(w, height));
  static_cast<CanvasRenderingContext2D*>(e->canvas->RenderingContext())
      ->reset();
  e->width = w;
  e->height = height;
  e->handle.version = ++version_;
  return {{}, e->handle};
}
WebPythonDomResult PythonCanvasBridgeImpl::Release(WebPythonCanvasHandle h) {
  auto* e = Resolve(h);
  if (!e) {
    return Expired();
  }
  if (!e->owned) {
    return {
        {}, Error::kNotAllowed, "Only managed cache surfaces can be released"};
  }
  e->owned->SetSize(gfx::Size());
  entries_[static_cast<wtf_size_t>(h.id - 1)] = nullptr;
  return {};
}
WebPythonCanvasResult PythonCanvasBridgeImpl::Submit(
    WebPythonCanvasHandle h,
    const std::vector<WebPythonCanvasCommand>& commands,
    base::FunctionRef<bool()> active) {
  const auto start = base::TimeTicks::Now();
  auto* entry = Resolve(h);
  if (!entry) {
    return {Expired()};
  }
  PythonCanvasCost cost;
  auto status =
      ValidatePythonCanvasBatch(commands, entry->width, entry->height, cost);
  if (status.error != Error::kNone) {
    return {status};
  }
  for (const auto& c : commands) {
    if (c.op != Op::kDrawSurface) {
      continue;
    }
    auto* source = Resolve(c.source);
    if (!source || source == entry) {
      return {{{}, Error::kReference, "Invalid Canvas source or self-copy"}};
    }
    cost.statistics.pixels += uint64_t(source->width) * source->height;
  }
  // Complete all validation and reserve the actual work before changing pixels.
  status = budget_.Consume(cost);
  if (status.error != Error::kNone) {
    return {status};
  }
  auto* ctx =
      static_cast<CanvasRenderingContext2D*>(entry->canvas->RenderingContext());
  if (!active() || ctx->isContextLost()) {
    return {{{}, Error::kState, "Canvas is inactive or its context was lost"}};
  }
  if (!Execute(*ctx, commands, active)) {
    return {{{},
             Error::kState,
             "Canvas submission stopped during native execution"}};
  }
  cost.statistics.native_ms =
      (base::TimeTicks::Now() - start).InMillisecondsF();
  return {{}, h, cost.statistics};
}

bool PythonCanvasBridgeImpl::Execute(
    CanvasRenderingContext2D& ctx,
    const std::vector<WebPythonCanvasCommand>& commands,
    base::FunctionRef<bool()> active) {
  HeapVector<Member<CanvasGradient>> gradients(33);
  ExceptionState exception(nullptr);
  ctx.reset();
  for (const auto& c : commands) {
    if (!active() || ctx.isContextLost()) {
      return false;
    }
    const auto& a = c.args;
    const String text = String::FromUtf8(c.text);
    switch (c.op) {
      case Op::kClearRect:
        ctx.clearRect(a[0], a[1], a[2], a[3]);
        break;
      case Op::kFillRect:
        ctx.fillRect(a[0], a[1], a[2], a[3]);
        break;
      case Op::kStrokeRect:
        ctx.strokeRect(a[0], a[1], a[2], a[3]);
        break;
      case Op::kBeginPath:
        ctx.beginPath();
        break;
      case Op::kClosePath:
        ctx.closePath();
        break;
      case Op::kMoveTo:
        ctx.moveTo(a[0], a[1]);
        break;
      case Op::kLineTo:
        ctx.lineTo(a[0], a[1]);
        break;
      case Op::kArc:
        ctx.arc(a[0], a[1], a[2], a[3], a[4], a[5], exception);
        break;
      case Op::kEllipse:
        ctx.ellipse(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], exception);
        break;
      case Op::kFill:
        ctx.fill();
        break;
      case Op::kStroke:
        ctx.stroke();
        break;
      case Op::kSave:
        ctx.save();
        break;
      case Op::kRestore:
        ctx.restore(exception);
        break;
      case Op::kTranslate:
        ctx.translate(a[0], a[1]);
        break;
      case Op::kScale:
        ctx.scale(a[0], a[1]);
        break;
      case Op::kRotate:
        ctx.rotate(a[0]);
        break;
      case Op::kFillColor:
        ctx.GetState().SetFillColor(ParseColor(c.text));
        ctx.GetState().ClearUnparsedFillColor();
        ctx.GetState().ClearResolvedFilter();
        break;
      case Op::kStrokeColor:
        ctx.GetState().SetStrokeColor(ParseColor(c.text));
        ctx.GetState().ClearUnparsedStrokeColor();
        ctx.GetState().ClearResolvedFilter();
        break;
      case Op::kFillGradient:
        ctx.GetState().SetFillGradient(gradients[c.index]);
        ctx.GetState().ClearUnparsedFillColor();
        ctx.GetState().ClearResolvedFilter();
        break;
      case Op::kStrokeGradient:
        ctx.GetState().SetStrokeGradient(gradients[c.index]);
        ctx.GetState().ClearUnparsedStrokeColor();
        ctx.GetState().ClearResolvedFilter();
        break;
      case Op::kLinearGradient:
        gradients[c.index] = ctx.createLinearGradient(a[0], a[1], a[2], a[3]);
        break;
      case Op::kColorStop:
        gradients[c.index]->addColorStop(a[0], text, exception);
        break;
      case Op::kLineWidth:
        ctx.setLineWidth(a[0]);
        break;
      case Op::kLineDash: {
        Vector<double> dash;
        for (double n : c.values) {
          dash.push_back(n);
        }
        ctx.setLineDash(dash);
        break;
      }
      case Op::kAlpha:
        ctx.setGlobalAlpha(a[0]);
        break;
      case Op::kFont:
        ctx.setFont(
            StrCat({a[1] ? "bold " : "", String::Number(a[0]), "px ", text}));
        break;
      case Op::kTextAlign: {
        using Align = V8CanvasTextAlign::Enum;
        ctx.setTextAlign(V8CanvasTextAlign(c.text == "left"     ? Align::kLeft
                                           : c.text == "right"  ? Align::kRight
                                           : c.text == "center" ? Align::kCenter
                                           : c.text == "start"  ? Align::kStart
                                                                : Align::kEnd));
        break;
      }
      case Op::kTextBaseline: {
        using Baseline = V8CanvasTextBaseline::Enum;
        ctx.setTextBaseline(V8CanvasTextBaseline(
            c.text == "top"           ? Baseline::kTop
            : c.text == "hanging"     ? Baseline::kHanging
            : c.text == "middle"      ? Baseline::kMiddle
            : c.text == "alphabetic"  ? Baseline::kAlphabetic
            : c.text == "ideographic" ? Baseline::kIdeographic
                                      : Baseline::kBottom));
        break;
      }
      case Op::kFillText:
        ctx.fillText(text, a[0], a[1]);
        break;
      case Op::kSmoothing:
        ctx.setImageSmoothingEnabled(a[0]);
        break;
      case Op::kDrawSurface: {
        auto* source = Resolve(c.source);
        if (!source) {
          return false;
        }
        ctx.drawImage(static_cast<CanvasImageSource*>(source->canvas.Get()), 0,
                      0, source->width, source->height, a[0], a[1], a[2], a[3],
                      exception);
        break;
      }
    }
    if (exception.HadException()) {
      return false;
    }
  }
  return active() && !ctx.isContextLost();
}
}  // namespace blink
