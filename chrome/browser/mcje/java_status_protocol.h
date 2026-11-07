// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCJE_JAVA_STATUS_PROTOCOL_H_
#define CHROME_BROWSER_MCJE_JAVA_STATUS_PROTOCOL_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/types/expected.h"
#include "chrome/browser/mcje/java_status_json.h"
#include "chrome/browser/mcje/mcje_url.h"

namespace mcje {

inline constexpr int32_t kProbeProtocolVersion = 47;
inline constexpr size_t kMaxSlpFrameBytes = 1024 * 1024;
inline constexpr size_t kMaxSlpInputBytes = 2 * 1024 * 1024;
inline constexpr size_t kMaxIgnoredSlpPackets = 8;

enum class VarIntStatus { kIncomplete, kValid, kInvalid };
struct VarIntResult {
  VarIntStatus status = VarIntStatus::kIncomplete;
  int32_t value = 0;
  size_t bytes_consumed = 0;
};

VarIntResult DecodeVarInt(base::span<const uint8_t> input);
std::vector<uint8_t> EncodeVarInt(int32_t value);
base::expected<std::vector<uint8_t>, std::string> EncodeHandshake(
    const ServerAddress& address);
std::vector<uint8_t> EncodeStatusRequest();
std::vector<uint8_t> EncodePing(uint64_t correlation);

enum class SlpError {
  kInvalidVarInt,
  kFrameLength,
  kInputLimit,
  kPacketLayout,
  kDuplicateStatus,
  kIgnoredPacketLimit,
  kInvalidJson,
  kTruncatedInput,
  kMissingStatus,
};

struct SlpFailure {
  SlpError code;
  std::string message;
};

// Incremental, uncompressed status-state receiver. Has no socket, clock, DNS or
// HTML dependencies. Errors are sticky; a prior valid status remains available
// so the network layer can retain it when a subsequent Pong/connection fails.
class JavaStatusStreamParser {
 public:
  explicit JavaStatusStreamParser(uint64_t expected_pong);
  ~JavaStatusStreamParser();

  bool Feed(base::span<const uint8_t> input);
  // End-of-stream: status without Pong is usable, incomplete frames are errors.
  bool Finish();

  const std::optional<JavaServerStatus>& status() const { return status_; }
  bool pong_received() const { return pong_received_; }
  bool complete() const {
    return !error_ && status_.has_value() && pong_received_;
  }
  const std::optional<SlpFailure>& error() const { return error_; }
  size_t bytes_received() const { return bytes_received_; }
  size_t ignored_packets() const { return ignored_packets_; }

 private:
  bool ProcessFrame();
  bool IgnorePacket();
  bool Fail(SlpError code, std::string message);

  const uint64_t expected_pong_;
  size_t bytes_received_ = 0;
  size_t ignored_packets_ = 0;
  std::array<uint8_t, 5> length_prefix_ = {};
  size_t prefix_size_ = 0;
  size_t frame_size_ = 0;
  std::vector<uint8_t> frame_;
  std::optional<JavaServerStatus> status_;
  bool pong_received_ = false;
  bool finished_ = false;
  std::optional<SlpFailure> error_;
};

}  // namespace mcje

#endif  // CHROME_BROWSER_MCJE_JAVA_STATUS_PROTOCOL_H_
