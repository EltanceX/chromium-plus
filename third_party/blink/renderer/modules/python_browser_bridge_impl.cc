// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/python_browser_bridge_impl.h"

#include <cmath>

#include "base/containers/span.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/renderer/bindings/core/v8/script_function.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_fullscreen_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_audio_context_options.h"
#include "third_party/blink/renderer/core/dom/attribute.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/document_type.h"
#include "third_party/blink/renderer/core/dom/node_traversal.h"
#include "third_party/blink/renderer/core/dom/processing_instruction.h"
#include "third_party/blink/renderer/core/dom/shadow_root.h"
#include "third_party/blink/renderer/core/editing/serializers/markup_accumulator.h"
#include "third_party/blink/renderer/core/fileapi/blob.h"
#include "third_party/blink/renderer/core/fileapi/public_url_manager.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/fullscreen/fullscreen.h"
#include "third_party/blink/renderer/core/html/anchor_element_utils.h"
#include "third_party/blink/renderer/core/html/html_anchor_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/modules/storage/dom_window_storage.h"
#include "third_party/blink/renderer/modules/storage/storage_area.h"
#include "third_party/blink/renderer/modules/webaudio/audio_context.h"
#include "third_party/blink/renderer/modules/webaudio/gain_node.h"
#include "third_party/blink/renderer/modules/webaudio/oscillator_handler.h"
#include "third_party/blink/renderer/modules/webaudio/oscillator_node.h"
#include "third_party/blink/renderer/modules/webaudio/realtime_audio_destination_node.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_request.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {
namespace {
using Op = WebPythonBrowserOp;
using Result = WebPythonBrowserResult;
Result Error(WebPythonDomError error, const char* text) {
  return {{{}, error, WebString::FromUtf8(text)}};
}
Result Denied(const char* text) {
  return Error(WebPythonDomError::kNotAllowed, text);
}
Result Limit(const char* text) {
  return Error(WebPythonDomError::kLimit, text);
}
void DestroyAudio(AudioContext* audio) {
  if (audio && audio->GetExecutionContext()) {
    // AudioContext::Uninitialize decrements the process context count and is
    // not idempotent. Detach its observer after early native cleanup, so a
    // subsequent document notification cannot uninitialize it a second time.
    audio->ContextDestroyed();
    audio->SetExecutionContext(nullptr);
  }
}
class Fulfilled final : public ThenCallable<IDLUndefined, Fulfilled> {
 public:
  Fulfilled(PythonBrowserBridgeImpl* owner, uint64_t id)
      : owner_(owner), id_(id) {}
  void React(ScriptState*) {
    if (owner_) {
      owner_->Settled(id_, true, "completed");
    }
  }
  void Trace(Visitor* v) const override {
    v->Trace(owner_);
    ThenCallable<IDLUndefined, Fulfilled>::Trace(v);
  }

 private:
  WeakMember<PythonBrowserBridgeImpl> owner_;
  uint64_t id_;
};
class Rejected final : public ThenCallable<IDLAny, Rejected> {
 public:
  Rejected(PythonBrowserBridgeImpl* owner, uint64_t id)
      : owner_(owner), id_(id) {}
  void React(ScriptState*, ScriptValue) {
    if (owner_) {
      owner_->Settled(id_, false, "Browser rejected the request");
    }
  }
  void Trace(Visitor* v) const override {
    v->Trace(owner_);
    ThenCallable<IDLAny, Rejected>::Trace(v);
  }

 private:
  WeakMember<PythonBrowserBridgeImpl> owner_;
  uint64_t id_;
};
class ExportMarkup final : public MarkupAccumulator {
 public:
  ExportMarkup()
      : MarkupAccumulator(ResolveUrls::kNone,
                          SerializationType::kHtml,
                          ShadowRootInclusion()) {}

 private:
  void AppendAttribute(const Element& e, const Attribute& a) override {
    // The parser hides nonce attributes; their internal value is still needed
    // in a self-contained download governed by the same CSP.
    if (a.GetName() == html_names::kNonceAttr) {
      MarkupAccumulator::AppendAttribute(e, Attribute(a.GetName(), e.nonce()));
    } else if (!(e.HasTagName(html_names::kDialogTag) &&
                 a.GetName() == html_names::kOpenAttr)) {
      MarkupAccumulator::AppendAttribute(e, a);
    }
  }
};
bool ValidTone(const WebPythonAudioTone& t) {
  const String wave = t.wave;
  return (wave == "sine" || wave == "square" || wave == "triangle" ||
          wave == "sawtooth") &&
         std::isfinite(t.frequency) && std::isfinite(t.end_frequency) &&
         std::isfinite(t.duration) && std::isfinite(t.delay) &&
         std::isfinite(t.volume) && t.frequency >= 20 && t.frequency <= 20000 &&
         t.end_frequency >= 20 && t.end_frequency <= 20000 &&
         t.duration >= .01 && t.duration <= 2 && t.delay >= 0 && t.delay <= 4 &&
         t.volume >= 0 && t.volume <= .25;
}
}  // namespace

PythonBrowserBridge* PythonBrowserBridgeImpl::Create(
    Document& d,
    base::RepeatingCallback<bool()> allowed) {
  return MakeGarbageCollected<PythonBrowserBridgeImpl>(d, std::move(allowed));
}
PythonBrowserBridgeImpl::PythonBrowserBridgeImpl(
    Document& d,
    base::RepeatingCallback<bool()> allowed)
    : document_(&d), allowed_(std::move(allowed)) {}
void PythonBrowserBridgeImpl::Voice::Trace(Visitor* v) const {
  v->Trace(oscillator);
  v->Trace(gain);
}
void PythonBrowserBridgeImpl::Trace(Visitor* v) const {
  v->Trace(document_);
  v->Trace(audio_);
  v->Trace(voices_);
  PythonBrowserBridge::Trace(v);
}
void PythonBrowserBridgeImpl::StopVoices() {
  for (auto& voice : voices_) {
    voice->gain->disconnect();
    voice->oscillator->stop(IGNORE_EXCEPTION);
    voice->oscillator->disconnect();
  }
  voices_.clear();
}
void PythonBrowserBridgeImpl::PruneVoices() {
  for (wtf_size_t i = voices_.size(); i > 0; --i) {
    if (voices_[i - 1]->end <= audio_->currentTime()) {
      voices_[i - 1]->gain->disconnect();
      voices_[i - 1]->oscillator->disconnect();
      voices_.EraseAt(i - 1);
    }
  }
}
void PythonBrowserBridgeImpl::Dispose() {
  if (disposed_) {
    return;
  }
  disposed_ = true;
  StopVoices();
  // Safe during native document teardown and fatal-stop/GC cleanup. No Python,
  // JS evaluation, promise entry or script-state creation is permitted here.
  DestroyAudio(audio_);
  audio_ = nullptr;
  pending_.clear();
  unsettled_.clear();
  completed_.clear();
  if (!export_url_.empty() && document_->domWindow()) {
    document_->domWindow()->GetPublicURLManager().Revoke(KURL(export_url_));
  }
  export_url_ = String();
}
uint64_t PythonBrowserBridgeImpl::Reserve() {
  if (pending_.size() + completed_.size() >= 8 || unsettled_.size() >= 8) {
    return 0;
  }
  const uint64_t id = ++next_id_;
  pending_.insert(id, base::TimeTicks::Now());
  return id;
}
void PythonBrowserBridgeImpl::Complete(uint64_t id,
                                       bool success,
                                       const String& detail) {
  if (disposed_ || !pending_.Contains(id)) {
    return;
  }
  pending_.erase(id);
  completed_.push_back(Result{{}, detail, id, success});
}
void PythonBrowserBridgeImpl::Observe(uint64_t id,
                                      ScriptState* state,
                                      ScriptPromise<IDLUndefined> promise) {
  unsettled_.insert(id);
  promise.Then(state, MakeGarbageCollected<Fulfilled>(this, id),
               MakeGarbageCollected<Rejected>(this, id));
}

void PythonBrowserBridgeImpl::Settled(uint64_t id,
                                      bool success,
                                      const String& detail) {
  unsettled_.erase(id);
  Complete(id, success, detail);
}

Result PythonBrowserBridgeImpl::Audio(const WebPythonBrowserRequest& r) {
  if (r.op == Op::kAudioState) {
    return {
        {},
        audio_ ? WebString(audio_->state().AsString()) : WebString("closed")};
  }
  if (r.op == Op::kAudioStop) {
    StopVoices();
    return {};
  }
  ExceptionState exception(nullptr);
  if (r.tones.empty() || r.tones.size() > 16) {
    return Limit("Expected 1 to 16 tones");
  }
  for (const auto& t : r.tones) {
    if (!ValidTone(t)) {
      return Limit("Invalid synthesis tone");
    }
  }
  if (!audio_ ||
      audio_->ContextState() != V8AudioContextState::Enum::kRunning) {
    return Error(WebPythonDomError::kState, "Audio is not running");
  }
  PruneVoices();
  const auto now = base::TimeTicks::Now();
  if (now - tone_window_ >= base::Seconds(1)) {
    tone_window_ = now;
    tone_count_ = 0;
  }
  if (voices_.size() + r.tones.size() > 16 ||
      tone_count_ + r.tones.size() > 64) {
    return Limit(
        "Audio allows 16 live voices and 64 scheduled tones per second");
  }
  tone_count_ += r.tones.size();
  const double base_time = audio_->currentTime();
  for (const auto& t : r.tones) {
    auto* v = MakeGarbageCollected<Voice>();
    v->oscillator = OscillatorNode::Create(*audio_, t.wave, nullptr, exception);
    v->gain = GainNode::Create(*audio_, exception);
    const double start = base_time + t.delay;
    v->end = start + t.duration;
    if (!v->oscillator || !v->gain || exception.HadException()) {
      StopVoices();
      return Error(WebPythonDomError::kState,
                   "Unable to create synthesis nodes");
    }
    v->oscillator->frequency()->setValueAtTime(t.frequency, start, exception);
    v->oscillator->frequency()->exponentialRampToValueAtTime(t.end_frequency,
                                                             v->end, exception);
    // A short attack and bounded decay avoid discontinuities. A fixed .25
    // master gain bounds the sum of sixteen .25-volume voices to unity.
    v->gain->gain()->setValueAtTime(0, start, exception);
    v->gain->gain()->linearRampToValueAtTime(t.volume * .25, start + .003,
                                             exception);
    v->gain->gain()->exponentialRampToValueAtTime(.00001, v->end, exception);
    v->oscillator->connect(v->gain, 0, 0, exception);
    v->gain->connect(audio_->destinationNode(), 0, 0, exception);
    v->oscillator->start(start, exception);
    v->oscillator->stop(v->end, exception);
    voices_.push_back(v);
    if (exception.HadException()) {
      StopVoices();
      return Error(WebPythonDomError::kState, "Unable to schedule synthesis");
    }
  }
  return {};
}

Result PythonBrowserBridgeImpl::Export(const String& filename) {
  if (filename.empty() || filename.length() > 96 ||
      !filename.ends_with(".html")) {
    return Limit("Expected a simple .html filename");
  }
  for (const auto c : filename.Utf8()) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) {
      return Limit("Download filename must not contain a path");
    }
  }
  if (!LocalFrame::HasTransientUserActivation(document_->GetFrame())) {
    return Denied("Export requires real user activation");
  }
  auto now = base::TimeTicks::Now();
  if (exports_ >= 8 ||
      (!last_export_.is_null() && now - last_export_ < base::Seconds(2))) {
    return Limit("Export allows 8 downloads, at least 2 seconds apart");
  }
  // Preflight before allocating markup: no uncounted template/author trees.
  // Native control shadow trees are not serialized by MarkupAccumulator.
  // at most 4096 nodes / 128 Ki UTF-16 units, hence bounded escaping growth.
  size_t nodes = 0, units = 0;
  for (Node* n = document_; n; n = NodeTraversal::Next(*n)) {
    if (++nodes > 4096) {
      return Limit("Document has too many export nodes");
    }
    units += n->nodeValue().length();
    unsigned depth = 0;
    for (Node* parent = n->parentNode(); parent;
         parent = parent->parentNode()) {
      if (++depth > 128) {
        return Limit("Document export nesting exceeds 128");
      }
    }
    if (auto* e = DynamicTo<Element>(n)) {
      const auto* shadow = e->GetShadowRoot();
      if ((shadow && !shadow->IsUserAgent()) ||
          e->HasTagName(html_names::kTemplateTag)) {
        return Denied("Template and shadow content export is unsupported");
      }
      units += e->tagName().length() * 2 + 8;
      units += e->nonce().length();
      for (const auto& a : e->Attributes()) {
        units += a.GetName().ToString().length() + a.Value().length() + 4;
      }
    } else if (auto* type = DynamicTo<DocumentType>(n)) {
      // These fields are absent from nodeValue(), but are emitted by the
      // serializer. Count them before allocating the document markup.
      units += type->name().length() + type->publicId().length() +
               type->systemId().length() + 32;
    } else if (auto* instruction = DynamicTo<ProcessingInstruction>(n)) {
      units += instruction->target().length() + 5;
    }
    if (units > 128 * 1024) {
      return Limit("Document exceeds the export source limit");
    }
  }
  ExportMarkup accumulator;
  const auto text =
      accumulator.SerializeNodes<EditingStrategy>(*document_, kIncludeNode)
          .Utf8();
  if (text.size() > 1024 * 1024) {
    return Limit("HTML export exceeds 1 MiB");
  }
  auto* window = document_->domWindow();
  if (!export_url_.empty()) {
    window->GetPublicURLManager().Revoke(KURL(export_url_));
  }
  auto* blob =
      Blob::Create(base::as_byte_span(text), "text/html;charset=utf-8");
  export_url_ = window->GetPublicURLManager().RegisterUrl(blob);
  const KURL url(export_url_);
  auto* anchor = MakeGarbageCollected<HTMLAnchorElement>(*document_);
  last_export_ = now;
  ++exports_;
  // Uses the same sandbox, navigation and browser download checks as <a
  // download>.
  AnchorElementUtils::HandleDownloadAttribute(anchor, filename, url, window,
                                              true, ResourceRequest(url));
  return {};  // Submitted to policy; this does not assert a file was saved.
}

Result PythonBrowserBridgeImpl::Run(const WebPythonBrowserRequest& r,
                                    Element* element) {
  if (disposed_ || !document_->IsActive() || !document_->GetFrame() ||
      !document_->domWindow()) {
    return Denied("Document is inactive");
  }
  if (r.op == Op::kPoll) {
    // Timeouts free their pending slot only when consumed. Late reactions are
    // ignored; completed + pending can never exceed eight entries.
    Vector<uint64_t> expired;
    for (const auto& p : pending_) {
      if (base::TimeTicks::Now() - p.value > base::Seconds(5)) {
        expired.push_back(p.key);
      }
    }
    for (uint64_t id : expired) {
      Complete(id, false, "Browser operation timed out");
    }
    if (completed_.empty()) {
      return {};
    }
    auto result = completed_.front();
    completed_.pop_front();
    return result;
  }
  if (r.op == Op::kStorageGet || r.op == Op::kStorageSet) {
    const String key = r.key, value = r.value;
    if (key.length() > 256 || value.length() > 16384 ||
        key.Utf8().size() > 256 || value.Utf8().size() > 16384) {
      return Limit("Storage key/value exceeds the bridge limit");
    }
    ExceptionState exception(nullptr);
    auto* storage =
        DOMWindowStorage::localStorage(*document_->domWindow(), exception);
    if (!storage || exception.HadException()) {
      return Denied("Local storage access denied");
    }
    if (r.op == Op::kStorageSet) {
      storage->setItem(key, value, exception);
      // QuotaExceededErrorUpdate's exception helper returns early without a
      // V8 isolate. Verify the synchronous cache mutation as well, rather
      // than treating an unreported quota rejection as a successful write.
      if (!exception.HadException()) {
        const String written = storage->getItem(key, exception);
        if (written.IsNull() || written != value) {
          return Denied("Storage write denied or quota exceeded");
        }
      }
    } else {
      const auto result = storage->getItem(key, exception);
      if (!exception.HadException() && result.length() <= 16384 &&
          result.Utf8().size() <= 16384) {
        return {{}, result, 0, !result.IsNull()};
      }
      if (!exception.HadException()) {
        return Limit("Stored value exceeds 16 KiB");
      }
    }
    return exception.HadException() ? Denied("Storage denied or quota exceeded")
                                    : Result{};
  }
  if (r.op == Op::kIsFullscreen) {
    return {
        {}, {}, 0, Fullscreen::FullscreenElementFrom(*document_) != nullptr};
  }
  if (r.op == Op::kExportDocument) {
    return Export(r.key);
  }
  if (r.op == Op::kAudioStop || r.op == Op::kAudioState ||
      r.op == Op::kPlayTones) {
    return Audio(r);
  }
  if (r.op == Op::kAudioResume && !audio_unlocked_ &&
      !LocalFrame::HasTransientUserActivation(document_->GetFrame())) {
    return Denied("Audio resume requires real user activation");
  }
  if (r.op != Op::kAudioResume && r.op != Op::kAudioSuspend &&
      r.op != Op::kRequestFullscreen && r.op != Op::kExitFullscreen) {
    return Denied("Unsupported browser operation");
  }
  if (r.op == Op::kRequestFullscreen &&
      (!element || &element->GetDocument() != document_ ||
       !element->isConnected())) {
    return Denied("Fullscreen target is inactive");
  }
  const uint64_t id = Reserve();
  if (!id) {
    return Limit("Consume pending browser results before requesting more");
  }
  if (r.op == Op::kAudioSuspend) {
    StopVoices();
  }
  document_->GetTaskRunner(TaskType::kInternalDefault)
      ->PostTask(FROM_HERE, BindOnce(&PythonBrowserBridgeImpl::BeginAsync,
                                     WrapWeakPersistent(this), id, r.op,
                                     WrapWeakPersistent(element)));
  return {{}, {}, id};
}

void PythonBrowserBridgeImpl::BeginAsync(uint64_t id, Op op, Element* element) {
  if (disposed_ || !pending_.Contains(id)) {
    return;
  }
  if (!allowed_ || !allowed_.Run() || !document_->IsActive() ||
      !document_->GetFrame() || !document_->domWindow()) {
    Dispose();
    return;
  }
  if (base::TimeTicks::Now() - pending_.at(id) > base::Seconds(5)) {
    Complete(id, false, "Browser operation timed out");
    return;
  }
  if (op == Op::kRequestFullscreen &&
      (!element || &element->GetDocument() != document_ ||
       !element->isConnected())) {
    Complete(id, false, "Fullscreen target is inactive");
    return;
  }
  if (op == Op::kAudioResume && !audio_unlocked_ &&
      !LocalFrame::HasTransientUserActivation(document_->GetFrame())) {
    Complete(id, false, "Audio activation expired before initialization");
    return;
  }
  // Initializing a V8 realm/audio output may be expensive on a cold browser.
  // Run bounded native setup outside Python, with live policy/activation
  // checks.
  v8::HandleScope handles(document_->GetExecutionContext()->GetIsolate());
  ScriptState* state = ToScriptStateForMainWorld(document_->GetFrame());
  if (!state || !state->ContextIsValid()) {
    Complete(id, false, "Script context is inactive");
    return;
  }
  ScriptState::Scope scope(state);
  ExceptionState exception(nullptr);
  if (op == Op::kAudioResume || op == Op::kAudioSuspend) {
    if (!audio_ && op == Op::kAudioResume) {
      audio_ = AudioContext::Create(document_->GetExecutionContext(),
                                    AudioContextOptions::Create(), exception);
      if (!audio_ || exception.HadException()) {
        Complete(id, false, "Unable to create audio output");
        return;
      }
      // PeriodicWave tables are otherwise built lazily by the first tone.
      // Prepare the fixed four supported waveforms in this bounded native
      // initialization task, before reporting readiness to a Python frame.
      for (int type :
           {OscillatorHandler::SINE, OscillatorHandler::SQUARE,
            OscillatorHandler::SAWTOOTH, OscillatorHandler::TRIANGLE}) {
        if (!audio_->GetPeriodicWave(type)) {
          DestroyAudio(audio_);
          audio_ = nullptr;
          Complete(id, false, "Unable to prepare synthesis tables");
          return;
        }
      }
      audio_unlocked_ = true;
    }
    if (!audio_) {
      Complete(id, true, "suspended");
      return;
    }
    if (!allowed_.Run()) {
      Dispose();
      return;
    }
    auto promise = op == Op::kAudioResume
                       ? audio_->resumeContext(state, exception)
                       : audio_->suspendContext(state, exception);
    if (exception.HadException()) {
      Complete(id, false, "Audio transition failed");
    } else {
      Observe(id, state, promise);
    }
    return;
  }
  auto promise =
      op == Op::kRequestFullscreen
          ? Fullscreen::RequestFullscreen(*element, FullscreenOptions::Create(),
                                          FullscreenRequestType::kUnprefixed,
                                          state, &exception)
          : Fullscreen::ExitFullscreen(*document_, state, &exception);
  if (exception.HadException()) {
    Complete(id, false, "Fullscreen transition failed");
  } else {
    Observe(id, state, promise);
  }
}
}  // namespace blink
