// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PYTHON_RUNTIME_PYTHON_BROWSER_TYPES_H_
#define COMPONENTS_PYTHON_RUNTIME_PYTHON_BROWSER_TYPES_H_

#include <string>
#include <vector>

namespace native_python {
enum class BrowserOp {
  kAudioResume,
  kAudioSuspend,
  kAudioStop,
  kAudioState,
  kPlayTones,
  kStorageGet,
  kStorageSet,
  kRequestFullscreen,
  kExitFullscreen,
  kIsFullscreen,
  kExportDocument,
  kPoll,
};
struct AudioTone {
  std::string wave;
  double frequency = 440;
  double end_frequency = 440;
  double duration = 0.1;
  double delay = 0;
  double volume = 0.1;
};
}  // namespace native_python
#endif  // COMPONENTS_PYTHON_RUNTIME_PYTHON_BROWSER_TYPES_H_
