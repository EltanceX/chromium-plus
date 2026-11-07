// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/java_status_protocol.h"

#include <algorithm>
#include <bit>
#include <utility>

#include "base/strings/string_util.h"

namespace mcje {
namespace {

void AppendVarInt(int32_t value, std::vector<uint8_t>& output) {
  auto encoded = EncodeVarInt(value);
  output.insert(output.end(), encoded.begin(), encoded.end());
}

std::vector<uint8_t> Frame(std::vector<uint8_t> packet) {
  auto output = EncodeVarInt(static_cast<int32_t>(packet.size()));
  output.insert(output.end(), packet.begin(), packet.end());
  return output;
}

}  // namespace

VarIntResult DecodeVarInt(base::span<const uint8_t> input) {
  uint32_t value = 0;
  for (size_t i = 0; i < std::min(input.size(), size_t{5}); ++i) {
    const uint8_t byte = input[i];
    if (i == 4 && (byte & 0xf0)) {
      return {VarIntStatus::kInvalid, 0, i + 1};
    }
    value |= static_cast<uint32_t>(byte & 0x7f) << (7 * i);
    if (!(byte & 0x80)) {
      return {VarIntStatus::kValid, std::bit_cast<int32_t>(value), i + 1};
    }
  }
  return {};
}

std::vector<uint8_t> EncodeVarInt(int32_t value) {
  uint32_t remaining = static_cast<uint32_t>(value);
  std::vector<uint8_t> encoded;
  do {
    uint8_t byte = remaining & 0x7f;
    remaining >>= 7;
    encoded.push_back(byte | (remaining ? 0x80 : 0));
  } while (remaining);
  return encoded;
}

base::expected<std::vector<uint8_t>, std::string> EncodeHandshake(
    const ServerAddress& address) {
  if (address.logical_host.empty() || address.logical_host.size() > 255 ||
      address.logical_host.find('\0') != std::string::npos ||
      !base::IsStringUTF8(address.logical_host) || address.logical_port == 0) {
    return base::unexpected("Invalid logical handshake host or port.");
  }
  std::vector<uint8_t> packet = {0};
  AppendVarInt(kProbeProtocolVersion, packet);
  AppendVarInt(static_cast<int32_t>(address.logical_host.size()), packet);
  packet.insert(packet.end(), address.logical_host.begin(),
                address.logical_host.end());
  packet.push_back(address.logical_port >> 8);
  packet.push_back(address.logical_port & 0xff);
  AppendVarInt(1, packet);
  return Frame(std::move(packet));
}

std::vector<uint8_t> EncodeStatusRequest() {
  return {1, 0};
}

std::vector<uint8_t> EncodePing(uint64_t correlation) {
  std::vector<uint8_t> packet = {1};
  for (int shift = 56; shift >= 0; shift -= 8) {
    packet.push_back((correlation >> shift) & 0xff);
  }
  return Frame(std::move(packet));
}

JavaStatusStreamParser::JavaStatusStreamParser(uint64_t expected_pong)
    : expected_pong_(expected_pong) {}

JavaStatusStreamParser::~JavaStatusStreamParser() = default;

bool JavaStatusStreamParser::Feed(base::span<const uint8_t> input) {
  if (error_) {
    return false;
  }
  if (finished_) {
    return Fail(SlpError::kPacketLayout, "Input arrived after end-of-stream.");
  }
  if (input.size() > kMaxSlpInputBytes - bytes_received_) {
    return Fail(SlpError::kInputLimit,
                "Cumulative SLP input exceeds the limit.");
  }
  bytes_received_ += input.size();
  while (!input.empty()) {
    if (!frame_size_) {
      length_prefix_[prefix_size_++] = input.front();
      input = input.subspan<1>();
      const auto length =
          DecodeVarInt(base::span(length_prefix_).first(prefix_size_));
      if (length.status == VarIntStatus::kInvalid) {
        return Fail(SlpError::kInvalidVarInt, "Invalid frame-length VarInt.");
      }
      if (length.status == VarIntStatus::kIncomplete) {
        continue;
      }
      if (length.value <= 0 ||
          static_cast<size_t>(length.value) > kMaxSlpFrameBytes) {
        return Fail(SlpError::kFrameLength,
                    "Invalid or excessive frame length.");
      }
      frame_size_ = length.value;
      prefix_size_ = 0;
    }
    const size_t count = std::min(frame_size_ - frame_.size(), input.size());
    const auto part = input.first(count);
    frame_.insert(frame_.end(), part.begin(), part.end());
    input = input.subspan(count);
    if (frame_.size() == frame_size_) {
      if (!ProcessFrame()) {
        return false;
      }
      frame_.clear();
      frame_size_ = 0;
    }
  }
  return true;
}

bool JavaStatusStreamParser::Finish() {
  if (error_) {
    return false;
  }
  finished_ = true;
  if (prefix_size_ || frame_size_) {
    return Fail(SlpError::kTruncatedInput,
                "Connection ended inside an SLP frame.");
  }
  if (!status_) {
    return Fail(SlpError::kMissingStatus,
                "Connection ended without status JSON.");
  }
  return true;
}

bool JavaStatusStreamParser::ProcessFrame() {
  const auto id = DecodeVarInt(frame_);
  if (id.status != VarIntStatus::kValid || id.value < 0) {
    return Fail(SlpError::kPacketLayout, "Invalid packet ID.");
  }
  auto payload = base::span(frame_).subspan(id.bytes_consumed);
  if (id.value == 0) {
    if (status_) {
      return Fail(SlpError::kDuplicateStatus, "Duplicate status response.");
    }
    const auto length = DecodeVarInt(payload);
    if (length.status != VarIntStatus::kValid || length.value < 0 ||
        static_cast<size_t>(length.value) !=
            payload.size() - length.bytes_consumed) {
      return Fail(SlpError::kPacketLayout,
                  "Invalid status JSON string length.");
    }
    auto status = ParseJavaStatusJson(
        base::as_string_view(payload.subspan(length.bytes_consumed)));
    if (!status.has_value()) {
      return Fail(SlpError::kInvalidJson, std::move(status.error()));
    }
    status_ = std::move(*status);
    return true;
  }
  if (id.value == 1) {
    if (payload.size() != 8) {
      return Fail(SlpError::kPacketLayout,
                  "Pong must contain exactly eight bytes.");
    }
    uint64_t correlation = 0;
    for (uint8_t byte : payload) {
      correlation = (correlation << 8) | byte;
    }
    if (correlation == expected_pong_ && !pong_received_) {
      pong_received_ = true;
      return true;
    }
  }
  return IgnorePacket();
}

bool JavaStatusStreamParser::IgnorePacket() {
  if (++ignored_packets_ > kMaxIgnoredSlpPackets) {
    return Fail(SlpError::kIgnoredPacketLimit, "Too many ignored SLP packets.");
  }
  return true;
}

bool JavaStatusStreamParser::Fail(SlpError code, std::string message) {
  error_ = SlpFailure{code, std::move(message)};
  frame_.clear();
  return false;
}

}  // namespace mcje
