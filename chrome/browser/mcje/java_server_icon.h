// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCJE_JAVA_SERVER_ICON_H_
#define CHROME_BROWSER_MCJE_JAVA_SERVER_ICON_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "chrome/browser/mcje/java_status_json.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/data_decoder/public/mojom/image_decoder.mojom.h"

class SkBitmap;
namespace data_decoder {
class DataDecoder;
}

namespace mcje {

inline constexpr size_t kMaxIconPngBytes = 64 * 1024;
inline constexpr size_t kMaxIconBitmapBytes = 64 * 1024;

// Only the processor can supply a data URL. The renderer never consumes the
// untrusted favicon string from JavaServerStatus.
class JavaServerIcon {
 public:
  const std::string& data_url() const { return data_url_; }
  const std::string& diagnostic() const { return diagnostic_; }

 private:
  friend class JavaServerIconProcessor;
  std::string data_url_;
  std::string diagnostic_ = "服务器未提供图标";
};

// Single-use, navigation-owned processor. Invalid/missing attachments can
// complete synchronously. Cancel/destruction closes both service connections
// and drops the callback without delivering a result.
class JavaServerIconProcessor {
 public:
  using Callback = base::OnceCallback<void(JavaServerIcon)>;
  JavaServerIconProcessor();
  ~JavaServerIconProcessor();
  void Start(const JavaServerStatus& status, Callback callback);
  void Cancel();

 private:
  void OnDecoded(const SkBitmap& bitmap);
  void Fail(std::string diagnostic);
  void Finish(JavaServerIcon icon);

  bool started_ = false;
  Callback callback_;
  std::unique_ptr<data_decoder::DataDecoder> service_;
  mojo::Remote<data_decoder::mojom::ImageDecoder> decoder_;
  base::OneShotTimer timer_;
  base::WeakPtrFactory<JavaServerIconProcessor> weak_factory_{this};
};

}  // namespace mcje

#endif  // CHROME_BROWSER_MCJE_JAVA_SERVER_ICON_H_
