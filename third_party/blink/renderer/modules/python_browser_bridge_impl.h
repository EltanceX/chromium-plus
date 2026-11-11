// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_PYTHON_BROWSER_BRIDGE_IMPL_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_PYTHON_BROWSER_BRIDGE_IMPL_H_

#include "base/time/time.h"
#include "third_party/blink/renderer/core/html/python_browser_bridge.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_vector.h"
#include "third_party/blink/renderer/platform/wtf/deque.h"
#include "third_party/blink/renderer/platform/wtf/hash_map.h"
#include "third_party/blink/renderer/platform/wtf/hash_set.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {
class AudioContext;
class OscillatorNode;
class GainNode;
class ScriptState;
template <typename>
class ScriptPromise;
struct IDLUndefined;

class MODULES_EXPORT PythonBrowserBridgeImpl final
    : public PythonBrowserBridge {
 public:
  static PythonBrowserBridge* Create(Document&,
                                     base::RepeatingCallback<bool()>);
  explicit PythonBrowserBridgeImpl(
      Document&,
      base::RepeatingCallback<bool()> allowed = {});
  WebPythonBrowserResult Run(const WebPythonBrowserRequest&, Element*) override;
  void Dispose() override;
  void Silence() override { StopVoices(); }
  void Trace(Visitor*) const override;
  // Promise reactions only collect bounded ordinary data. Python is entered
  // later by its own frame/event scheduler, after policy and budget checks.
  void Complete(uint64_t id, bool success, const String& detail);
  void Settled(uint64_t id, bool success, const String& detail);

 private:
  struct Voice : public GarbageCollected<Voice> {
    Member<OscillatorNode> oscillator;
    Member<GainNode> gain;
    double end = 0;
    void Trace(Visitor*) const;
  };
  WebPythonBrowserResult Audio(const WebPythonBrowserRequest&);
  void BeginAsync(uint64_t, WebPythonBrowserOp, Element*);
  WebPythonBrowserResult Export(const String& filename);
  uint64_t Reserve();
  void Observe(uint64_t, ScriptState*, ScriptPromise<IDLUndefined>);
  void StopVoices();
  void PruneVoices();
  Member<Document> document_;
  base::RepeatingCallback<bool()> allowed_;
  Member<AudioContext> audio_;
  HeapVector<Member<Voice>> voices_;
  HashMap<uint64_t, base::TimeTicks> pending_;
  HashSet<uint64_t> unsettled_;
  Deque<WebPythonBrowserResult> completed_;
  uint64_t next_id_ = 0;
  bool disposed_ = false;
  bool audio_unlocked_ = false;
  String export_url_;
  base::TimeTicks last_export_;
  unsigned exports_ = 0;
  base::TimeTicks tone_window_;
  unsigned tone_count_ = 0;
};
}  // namespace blink
#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_PYTHON_BROWSER_BRIDGE_IMPL_H_
