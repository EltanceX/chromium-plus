// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/java_server_icon.h"

#include <optional>
#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "services/data_decoder/public/cpp/service_provider.h"
#include "services/data_decoder/public/mojom/gzipper.mojom.h"
#include "services/data_decoder/public/mojom/structured_headers_parser.mojom.h"
#include "services/data_decoder/public/mojom/xml_parser.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/size.h"

namespace mcje {
namespace {

constexpr std::string_view kPrefix = "data:image/png;base64,";

SkBitmap Bitmap(int size = 64) {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(size, size);
  bitmap.eraseColor(SkColorSetARGB(128, 40, 160, 80));
  return bitmap;
}

// Exercise real Mojo serialization and disconnection without requiring Blink
// initialization in this small test binary. Actual PNG decoding runs in the
// browser smoke test against the isolated service.
class FakeImageService : public data_decoder::ServiceProvider,
                         public data_decoder::mojom::DataDecoderService,
                         public data_decoder::mojom::ImageDecoder {
 public:
  FakeImageService() { data_decoder::ServiceProvider::Set(this); }
  ~FakeImageService() override { data_decoder::ServiceProvider::Set(nullptr); }

  void BindDataDecoderService(
      mojo::PendingReceiver<data_decoder::mojom::DataDecoderService> receiver)
      override {
    ++launches;
    service.Bind(std::move(receiver));
    service.set_disconnect_handler(
        base::BindOnce([](bool* closed) { *closed = true; }, &service_closed));
  }
  void BindImageDecoder(mojo::PendingReceiver<data_decoder::mojom::ImageDecoder>
                            receiver) override {
    image.Bind(std::move(receiver));
    image.set_disconnect_handler(
        base::BindOnce([](bool* closed) { *closed = true; }, &image_closed));
  }
  void DecodeImage(mojo_base::BigBuffer bytes,
                   data_decoder::mojom::ImageCodec codec,
                   bool shrink,
                   int64_t max_size,
                   const gfx::Size& desired,
                   DecodeImageCallback callback) override {
    EXPECT_EQ(data_decoder::mojom::ImageCodec::kDefault, codec);
    EXPECT_FALSE(shrink);
    EXPECT_EQ(kMaxIconBitmapBytes, static_cast<size_t>(max_size));
    EXPECT_EQ(64, desired.width());
    EXPECT_EQ(64, desired.height());
    input.assign(bytes.begin(), bytes.end());
    pending = std::move(callback);
  }
  void DecodeAnimation(mojo_base::BigBuffer,
                       bool,
                       int64_t,
                       DecodeAnimationCallback) override {
    ADD_FAILURE() << "Animations must not be requested";
  }
  void BindXmlParser(
      mojo::PendingReceiver<data_decoder::mojom::XmlParser>) override {
    ADD_FAILURE();
  }
  void BindGzipper(
      mojo::PendingReceiver<data_decoder::mojom::Gzipper>) override {
    ADD_FAILURE();
  }
  void BindStructuredHeadersParser(
      mojo::PendingReceiver<data_decoder::mojom::StructuredHeadersParser>)
      override {
    ADD_FAILURE();
  }

  int launches = 0;
  bool service_closed = false;
  bool image_closed = false;
  std::string input;
  DecodeImageCallback pending;
  mojo::Receiver<data_decoder::mojom::DataDecoderService> service{this};
  mojo::Receiver<data_decoder::mojom::ImageDecoder> image{this};
};

class JavaServerIconTest : public testing::Test {
 protected:
  void Start(std::optional<std::string> url) {
    JavaServerStatus status;
    status.favicon = std::move(url);
    processor.Start(
        status,
        base::BindOnce([](std::optional<JavaServerIcon>* output,
                          JavaServerIcon icon) { *output = std::move(icon); },
                       &result));
    tasks.RunUntilIdle();
  }
  std::string Url(std::string_view bytes) {
    return std::string(kPrefix) + base::Base64Encode(bytes);
  }
  void ExpectPreflightFailure(std::string url) {
    Start(std::move(url));
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->data_url().empty());
    EXPECT_EQ(0, service.launches);
  }

  base::test::TaskEnvironment tasks{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  FakeImageService service;
  JavaServerIconProcessor processor;
  std::optional<JavaServerIcon> result;
  const std::vector<uint8_t> png =
      *gfx::PNGCodec::EncodeBGRASkBitmap(Bitmap(), false);
};

TEST_F(JavaServerIconTest, MissingAttachmentDoesNotLaunchService) {
  Start(std::nullopt);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->data_url().empty());
  EXPECT_EQ(0, service.launches);
}

TEST_F(JavaServerIconTest, ExtractionErrorDoesNotLaunchService) {
  JavaServerStatus status;
  status.favicon_error = "wrong type";
  processor.Start(status, base::BindOnce([](JavaServerIcon icon) {
                    EXPECT_TRUE(icon.data_url().empty());
                    EXPECT_FALSE(icon.diagnostic().empty());
                  }));
  EXPECT_EQ(0, service.launches);
}

TEST_F(JavaServerIconTest, RejectsRemoteUrl) {
  ExpectPreflightFailure("https://example.test/icon.png");
}
TEST_F(JavaServerIconTest, RejectsOtherMime) {
  ExpectPreflightFailure("data:image/svg+xml;base64,PHN2Zz4=");
}
TEST_F(JavaServerIconTest, RequiresExactPrefix) {
  ExpectPreflightFailure("data:image/PNG;base64," + base::Base64Encode(png));
}
TEST_F(JavaServerIconTest, RejectsWhitespaceInBase64) {
  ExpectPreflightFailure(std::string(kPrefix) + " \n" +
                         base::Base64Encode(png));
}
TEST_F(JavaServerIconTest, RejectsMalformedBase64) {
  ExpectPreflightFailure(std::string(kPrefix) + "%%%%");
}
TEST_F(JavaServerIconTest, RejectsNonCanonicalPaddingBits) {
  std::string bytes(png.begin(), png.end());
  while (bytes.size() % 3 != 1) {
    bytes += 'x';
  }
  auto encoded = base::Base64Encode(bytes);
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  encoded[encoded.size() - 3] =
      alphabet[alphabet.find(encoded[encoded.size() - 3]) + 1];
  ExpectPreflightFailure(std::string(kPrefix) + encoded);
}
TEST_F(JavaServerIconTest, RejectsEncodedOversize) {
  ExpectPreflightFailure(std::string(kMaxFaviconDataUrlBytes + 1, 'A'));
}
TEST_F(JavaServerIconTest, RejectsDecodedOversizeWithinEncodedLimit) {
  // 65537 bytes has the same Base64 length as 65536 bytes.
  ExpectPreflightFailure(Url(std::string(kMaxIconPngBytes + 1, 'A')));
}
TEST_F(JavaServerIconTest, RejectsSignature) {
  std::string bytes(png.begin(), png.end());
  bytes[0] = 0;
  ExpectPreflightFailure(Url(bytes));
}
TEST_F(JavaServerIconTest, RejectsTruncatedIhdr) {
  ExpectPreflightFailure(Url(std::string(png.begin(), png.begin() + 32)));
}
TEST_F(JavaServerIconTest, RejectsWrongFirstChunk) {
  std::string bytes(png.begin(), png.end());
  bytes[12] = 'X';
  ExpectPreflightFailure(Url(bytes));
}
TEST_F(JavaServerIconTest, RejectsWrongIhdrLength) {
  std::string bytes(png.begin(), png.end());
  bytes[11] = 12;
  ExpectPreflightFailure(Url(bytes));
}
TEST_F(JavaServerIconTest, RejectsOtherDimensions) {
  ExpectPreflightFailure(std::string(kPrefix) +
                         base::Base64Encode(*gfx::PNGCodec::EncodeBGRASkBitmap(
                             Bitmap(32), false)));
}

TEST_F(JavaServerIconTest, ReencodesPixelsPreservingAlphaAndRemovingMetadata) {
  std::string input(png.begin(), png.end());
  input += "REMOTE_METADATA_MUST_NOT_REACH_PAGE";
  Start(Url(input));
  ASSERT_TRUE(service.pending);
  EXPECT_EQ(input, service.input);
  EXPECT_FALSE(result);
  std::move(service.pending).Run(Bitmap());
  tasks.RunUntilIdle();
  ASSERT_TRUE(result);
  EXPECT_TRUE(service.image_closed);
  EXPECT_TRUE(service.service_closed);
  const auto decoded = base::Base64Decode(
      std::string_view(result->data_url()).substr(kPrefix.size()));
  ASSERT_TRUE(decoded);
  EXPECT_LT(decoded->size(), kMaxIconPngBytes);
  EXPECT_EQ(
      std::string::npos,
      std::string(decoded->begin(), decoded->end()).find("REMOTE_METADATA"));
  // Trusted re-encoded output can be decoded in-process in this test.
  const SkBitmap bitmap = gfx::PNGCodec::Decode(*decoded);
  EXPECT_EQ(64, bitmap.width());
  EXPECT_EQ(64, bitmap.height());
  EXPECT_NEAR(128, SkColorGetA(bitmap.getColor(0, 0)), 1);
}

TEST_F(JavaServerIconTest, RejectsEmptyDecodedBitmap) {
  Start(std::string(kPrefix) + base::Base64Encode(png));
  ASSERT_TRUE(service.pending);
  std::move(service.pending).Run(SkBitmap());
  tasks.RunUntilIdle();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->data_url().empty());
}
TEST_F(JavaServerIconTest, RechecksDecodedDimensions) {
  Start(std::string(kPrefix) + base::Base64Encode(png));
  ASSERT_TRUE(service.pending);
  std::move(service.pending).Run(Bitmap(32));
  tasks.RunUntilIdle();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->data_url().empty());
}
TEST_F(JavaServerIconTest, DecoderDisconnectUsesPlaceholder) {
  Start(std::string(kPrefix) + base::Base64Encode(png));
  ASSERT_TRUE(service.pending);
  service.image.reset();
  tasks.RunUntilIdle();
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->data_url().empty());
  EXPECT_TRUE(service.service_closed);
}
TEST_F(JavaServerIconTest, TimeoutClosesServiceAndDropsLateResult) {
  Start(std::string(kPrefix) + base::Base64Encode(png));
  ASSERT_TRUE(service.pending);
  tasks.FastForwardBy(base::Milliseconds(1999));
  EXPECT_FALSE(result);
  tasks.FastForwardBy(base::Milliseconds(1));
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->data_url().empty());
  EXPECT_EQ("图标处理超时", result->diagnostic());
  EXPECT_TRUE(service.image_closed);
  EXPECT_TRUE(service.service_closed);
  std::move(service.pending).Run(Bitmap());
  tasks.RunUntilIdle();
  EXPECT_TRUE(result->data_url().empty());
}
TEST_F(JavaServerIconTest, CancelClosesBothConnectionsWithoutCallback) {
  Start(std::string(kPrefix) + base::Base64Encode(png));
  ASSERT_TRUE(service.pending);
  processor.Cancel();
  tasks.RunUntilIdle();
  EXPECT_TRUE(service.image_closed);
  EXPECT_TRUE(service.service_closed);
  std::move(service.pending).Run(Bitmap());
  tasks.FastForwardBy(base::Seconds(3));
  EXPECT_FALSE(result);
}

TEST_F(JavaServerIconTest, DestructionClosesPendingDecoderWithoutCallback) {
  {
    JavaServerIconProcessor owned;
    JavaServerStatus status;
    status.favicon = std::string(kPrefix) + base::Base64Encode(png);
    owned.Start(status, base::BindOnce([](JavaServerIcon) {
                  ADD_FAILURE() << "Destroyed processor must not complete";
                }));
    tasks.RunUntilIdle();
    ASSERT_TRUE(service.pending);
  }
  tasks.RunUntilIdle();
  EXPECT_TRUE(service.image_closed);
  EXPECT_TRUE(service.service_closed);
  std::move(service.pending).Run(Bitmap());
  tasks.FastForwardBy(base::Seconds(3));
}

}  // namespace
}  // namespace mcje
