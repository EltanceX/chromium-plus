// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/java_server_icon.h"

#include <array>
#include <string_view>
#include <utility>

#include "base/base64.h"
#include "base/check.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "services/data_decoder/public/cpp/data_decoder.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/size.h"

namespace mcje {
namespace {

constexpr std::string_view kPngPrefix = "data:image/png;base64,";
constexpr std::array<uint8_t, 16> kPngHeader = {
    137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
constexpr std::array<uint8_t, 8> kDimensions = {0, 0, 0, 64, 0, 0, 0, 64};

bool HasExpectedHeader(base::span<const uint8_t> bytes) {
  // Include all 13 IHDR bytes and its CRC. The isolated decoder validates
  // the remaining PNG structure, chunk contents and compressed pixels.
  return bytes.size() >= 33 && bytes.first<16>() == kPngHeader &&
         bytes.subspan<16, 8>() == kDimensions;
}

}  // namespace

JavaServerIconProcessor::JavaServerIconProcessor() = default;
JavaServerIconProcessor::~JavaServerIconProcessor() {
  Cancel();
}

void JavaServerIconProcessor::Start(const JavaServerStatus& status,
                                    Callback callback) {
  CHECK(!started_);
  started_ = true;
  callback_ = std::move(callback);
  if (status.favicon_error) {
    Fail("图标附件类型或长度无效");
    return;
  }
  if (!status.favicon) {
    Finish(JavaServerIcon());
    return;
  }
  const std::string_view url = *status.favicon;
  if (url.size() > kMaxFaviconDataUrlBytes || !url.starts_with(kPngPrefix)) {
    Fail("图标仅支持有界的 PNG data URL");
    return;
  }
  const auto encoded = url.substr(kPngPrefix.size());
  std::string bytes;
  if (!base::Base64Decode(encoded, &bytes, base::Base64DecodePolicy::kStrict) ||
      bytes.size() > kMaxIconPngBytes || base::Base64Encode(bytes) != encoded) {
    Fail("图标 Base64 无效或 PNG 超过 64 KiB");
    return;
  }
  if (!HasExpectedHeader(base::as_byte_span(bytes))) {
    Fail("图标必须是 64×64 PNG");
    return;
  }

  timer_.Start(FROM_HERE, base::Seconds(2),
               base::BindOnce(&JavaServerIconProcessor::Fail,
                              weak_factory_.GetWeakPtr(), "图标处理超时"));
  service_ = std::make_unique<data_decoder::DataDecoder>();
  service_->GetService()->BindImageDecoder(
      decoder_.BindNewPipeAndPassReceiver());
  decoder_.set_disconnect_handler(base::BindOnce(&JavaServerIconProcessor::Fail,
                                                 weak_factory_.GetWeakPtr(),
                                                 "图标解码服务已断开"));
  // Own the remotes explicitly so cancellation also releases the service;
  // callback-owned DecodeImageIsolated() would retain it until completion.
  decoder_->DecodeImage(
      base::as_byte_span(bytes), data_decoder::mojom::ImageCodec::kDefault,
      /*shrink_to_fit=*/false, kMaxIconBitmapBytes, gfx::Size(64, 64),
      base::BindOnce(&JavaServerIconProcessor::OnDecoded,
                     weak_factory_.GetWeakPtr()));
}

void JavaServerIconProcessor::OnDecoded(const SkBitmap& bitmap) {
  if (bitmap.isNull() || !bitmap.readyToDraw() || bitmap.width() != 64 ||
      bitmap.height() != 64 || bitmap.colorType() != kN32_SkColorType ||
      bitmap.computeByteSize() > kMaxIconBitmapBytes) {
    Fail("图标解码失败或位图无效");
    return;
  }
  // Encode just the decoded pixels, retaining alpha but no remote metadata
  // or animation. This is bounded to one 64x64 bitmap.
  auto png = gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, false);
  if (!png || png->size() > kMaxIconPngBytes || !HasExpectedHeader(*png)) {
    Fail("图标 PNG 重新编码失败");
    return;
  }
  JavaServerIcon icon;
  icon.data_url_ = std::string(kPngPrefix) + base::Base64Encode(*png);
  icon.diagnostic_ = "64×64 PNG";
  Finish(std::move(icon));
}

void JavaServerIconProcessor::Fail(std::string diagnostic) {
  JavaServerIcon icon;
  icon.diagnostic_ = std::move(diagnostic);
  Finish(std::move(icon));
}

void JavaServerIconProcessor::Finish(JavaServerIcon icon) {
  auto callback = std::move(callback_);
  Cancel();
  std::move(callback).Run(std::move(icon));
}

void JavaServerIconProcessor::Cancel() {
  weak_factory_.InvalidateWeakPtrs();
  timer_.Stop();
  decoder_.reset();
  service_.reset();
  callback_.Reset();
}

}  // namespace mcje
