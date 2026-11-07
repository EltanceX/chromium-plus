// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/java_status_protocol.h"

#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace mcje {
namespace {

constexpr uint64_t kCorrelation = 0xfedcba9876543210;

std::vector<uint8_t> FrameForTest(std::vector<uint8_t> packet) {
  auto frame = EncodeVarInt(static_cast<int32_t>(packet.size()));
  frame.insert(frame.end(), packet.begin(), packet.end());
  return frame;
}

std::vector<uint8_t> StatusFrame(std::string_view json) {
  std::vector<uint8_t> packet = {0};
  const auto length = EncodeVarInt(static_cast<int32_t>(json.size()));
  packet.insert(packet.end(), length.begin(), length.end());
  packet.insert(packet.end(), json.begin(), json.end());
  return FrameForTest(std::move(packet));
}

std::vector<uint8_t> Join(std::vector<uint8_t> first,
                          base::span<const uint8_t> second) {
  first.insert(first.end(), second.begin(), second.end());
  return first;
}

TEST(JavaVarIntTest, KnownWireValuesAndSignedRoundTrips) {
  const std::pair<int32_t, std::vector<uint8_t>> cases[] = {
      {0, {0}},
      {127, {0x7f}},
      {128, {0x80, 1}},
      {255, {0xff, 1}},
      {2147483647, {0xff, 0xff, 0xff, 0xff, 7}},
      {-1, {0xff, 0xff, 0xff, 0xff, 0x0f}},
      {std::numeric_limits<int32_t>::min(), {0x80, 0x80, 0x80, 0x80, 8}},
  };
  for (const auto& [value, wire] : cases) {
    EXPECT_EQ(wire, EncodeVarInt(value));
    auto result = DecodeVarInt(Join(wire, std::vector<uint8_t>{0xff}));
    EXPECT_EQ(VarIntStatus::kValid, result.status);
    EXPECT_EQ(value, result.value);
    EXPECT_EQ(wire.size(), result.bytes_consumed);
    for (size_t size = 0; size < wire.size(); ++size) {
      EXPECT_EQ(VarIntStatus::kIncomplete,
                DecodeVarInt(base::span(wire).first(size)).status);
    }
  }
}

TEST(JavaVarIntTest, RejectsFifthByteOverflowAndContinuation) {
  for (uint8_t last : {0x10, 0x70, 0x80, 0xff}) {
    const std::vector<uint8_t> wire = {0x80, 0x80, 0x80, 0x80, last, 0};
    EXPECT_EQ(VarIntStatus::kInvalid, DecodeVarInt(wire).status);
  }
}

TEST(JavaStatusEncoderTest, HandshakeHasLogicalTargetAndStatusNextState) {
  ServerAddress address;
  address.logical_host = "example.com";
  address.logical_port = 25565;
  const std::vector<uint8_t> expected = {0x11, 0,   47,  11,   'e',  'x',
                                         'a',  'm', 'p', 'l',  'e',  '.',
                                         'c',  'o', 'm', 0x63, 0xdd, 1};
  auto encoded = EncodeHandshake(address);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(expected, *encoded);
  // The encoder accepts only the logical address, independently of SRV/IP.
  address.logical_port = 25570;
  address.port_was_explicit = true;
  encoded = EncodeHandshake(address);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_EQ(0xe2, encoded->at(encoded->size() - 2));
}

TEST(JavaStatusEncoderTest, HandshakeChecksHostAndPortBoundaries) {
  ServerAddress address;
  address.logical_host = std::string(255, 'a');
  ASSERT_TRUE(EncodeHandshake(address).has_value());
  address.logical_host += 'a';
  EXPECT_FALSE(EncodeHandshake(address).has_value());
  for (const auto& host :
       {std::string(), std::string("a\0b", 3), std::string("\xff", 1)}) {
    address.logical_host = host;
    EXPECT_FALSE(EncodeHandshake(address).has_value());
  }
  address.logical_host = "::1";
  address.logical_port = 0;
  EXPECT_FALSE(EncodeHandshake(address).has_value());
  address.logical_port = 65535;
  EXPECT_TRUE(EncodeHandshake(address).has_value());
}

TEST(JavaStatusEncoderTest, RequestAndPingMatchKnownWireBytes) {
  EXPECT_EQ((std::vector<uint8_t>{1, 0}), EncodeStatusRequest());
  EXPECT_EQ((std::vector<uint8_t>{9, 1, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54,
                                  0x32, 0x10}),
            EncodePing(kCorrelation));
}

TEST(JavaStatusStreamTest, CoalescedStatusAndPongWorkInEitherOrder) {
  const auto status = StatusFrame(R"({"description":"Hello 世界"})");
  const auto pong = EncodePing(kCorrelation);
  for (const auto& wire : {Join(status, pong), Join(pong, status)}) {
    JavaStatusStreamParser parser(kCorrelation);
    ASSERT_TRUE(parser.Feed(wire));
    EXPECT_TRUE(parser.complete());
    ASSERT_TRUE(parser.status());
    EXPECT_EQ("Hello 世界", parser.status()->motd);
    EXPECT_TRUE(parser.Finish());
    EXPECT_EQ(wire.size(), parser.bytes_received());
  }
}

TEST(JavaStatusStreamTest, EverySplitAcrossLengthIdJsonAndPong) {
  const auto status =
      StatusFrame("{\"description\":\"" + std::string(180, 'x') + "世界\"}");
  const auto wire = Join(status, EncodePing(kCorrelation));
  for (size_t split = 0; split <= wire.size(); ++split) {
    SCOPED_TRACE(split);
    JavaStatusStreamParser parser(kCorrelation);
    ASSERT_TRUE(parser.Feed(base::span(wire).first(split)));
    ASSERT_TRUE(parser.Feed(base::span(wire).subspan(split)));
    EXPECT_TRUE(parser.complete());
    ASSERT_TRUE(parser.status());
    EXPECT_EQ(std::string(180, 'x') + "世界", parser.status()->motd);
  }
  JavaStatusStreamParser bytewise(kCorrelation);
  for (uint8_t byte : wire) {
    ASSERT_TRUE(bytewise.Feed(base::span_from_ref(byte)));
  }
  EXPECT_TRUE(bytewise.complete());
}

TEST(JavaStatusStreamTest, EveryEarlyCloseRetainsOnlyCompleteStatus) {
  const auto status = StatusFrame(R"({"description":"ok"})");
  const auto wire = Join(status, EncodePing(kCorrelation));
  for (size_t size = 0; size < wire.size(); ++size) {
    SCOPED_TRACE(size);
    JavaStatusStreamParser parser(kCorrelation);
    ASSERT_TRUE(parser.Feed(base::span(wire).first(size)));
    EXPECT_EQ(size >= status.size(), parser.status().has_value());
    EXPECT_FALSE(parser.pong_received());
    if (size == status.size()) {
      EXPECT_TRUE(parser.Finish());
    } else {
      EXPECT_FALSE(parser.Finish());
      ASSERT_TRUE(parser.error());
      EXPECT_EQ(size ? SlpError::kTruncatedInput : SlpError::kMissingStatus,
                parser.error()->code);
    }
  }
}

TEST(JavaStatusStreamTest, InvalidFrameLengthsAndVarIntsAreStickyErrors) {
  const std::pair<std::vector<uint8_t>, SlpError> cases[] = {
      {{0}, SlpError::kFrameLength},
      {EncodeVarInt(-1), SlpError::kFrameLength},
      {EncodeVarInt(kMaxSlpFrameBytes + 1), SlpError::kFrameLength},
      {{0x80, 0x80, 0x80, 0x80, 0x10}, SlpError::kInvalidVarInt},
      {{0x80, 0x80, 0x80, 0x80, 0x80, 0}, SlpError::kInvalidVarInt},
  };
  for (const auto& [wire, error] : cases) {
    JavaStatusStreamParser parser(kCorrelation);
    EXPECT_FALSE(parser.Feed(wire));
    ASSERT_TRUE(parser.error());
    EXPECT_EQ(error, parser.error()->code);
    EXPECT_FALSE(parser.Feed(StatusFrame(R"({"description":"ok"})")));
    EXPECT_FALSE(parser.Finish());
    EXPECT_EQ(error, parser.error()->code);
    EXPECT_FALSE(parser.status());
  }
}

TEST(JavaStatusStreamTest, KnownPacketsConsumeExactlyTheirPayload) {
  const std::vector<std::vector<uint8_t>> packets = {
      {0x80},
      {0xff, 0xff, 0xff, 0xff, 0x0f},
      {0x80, 0x80, 0x80, 0x80, 0x10},
      {0},
      {0, 0x80},
      {0, 0xff, 0xff, 0xff, 0xff, 0x0f},
      {0, 2, '{'},
      {0, 2, '{', '}', 0},
      {1, 0, 0, 0, 0, 0, 0, 0},
      {1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
  };
  for (const auto& packet : packets) {
    JavaStatusStreamParser parser(kCorrelation);
    EXPECT_FALSE(parser.Feed(FrameForTest(packet)));
    ASSERT_TRUE(parser.error());
    EXPECT_EQ(SlpError::kPacketLayout, parser.error()->code);
  }
}

TEST(JavaStatusStreamTest, InvalidStatusJsonHasSeparateErrorCategory) {
  JavaStatusStreamParser parser(kCorrelation);
  EXPECT_FALSE(parser.Feed(StatusFrame(R"({"description":true})")));
  ASSERT_TRUE(parser.error());
  EXPECT_EQ(SlpError::kInvalidJson, parser.error()->code);
}

TEST(JavaStatusStreamTest, PongCorrelationIsIndependentAndBounded) {
  JavaStatusStreamParser parser(kCorrelation);
  ASSERT_TRUE(parser.Feed(EncodePing(kCorrelation + 1)));
  EXPECT_FALSE(parser.pong_received());
  EXPECT_EQ(1u, parser.ignored_packets());
  ASSERT_TRUE(parser.Feed(EncodePing(kCorrelation)));
  EXPECT_TRUE(parser.pong_received());
  EXPECT_FALSE(parser.complete());
  EXPECT_FALSE(parser.status());
  ASSERT_TRUE(parser.Feed(StatusFrame(R"({"description":"ok"})")));
  EXPECT_TRUE(parser.complete());
}

TEST(JavaStatusStreamTest, UnknownAndUnmatchedPacketsHaveFixedLimit) {
  for (const auto& ignored :
       {FrameForTest({2}), EncodePing(kCorrelation + 1)}) {
    JavaStatusStreamParser parser(kCorrelation);
    for (size_t i = 0; i < kMaxIgnoredSlpPackets; ++i) {
      ASSERT_TRUE(parser.Feed(ignored));
    }
    EXPECT_FALSE(parser.Feed(ignored));
    ASSERT_TRUE(parser.error());
    EXPECT_EQ(SlpError::kIgnoredPacketLimit, parser.error()->code);
  }
}

TEST(JavaStatusStreamTest, DuplicateStatusDoesNotReplaceFirstResult) {
  JavaStatusStreamParser parser(kCorrelation);
  ASSERT_TRUE(parser.Feed(StatusFrame(R"({"description":"first"})")));
  EXPECT_FALSE(parser.Feed(StatusFrame(R"({"description":"second"})")));
  ASSERT_TRUE(parser.error());
  EXPECT_EQ(SlpError::kDuplicateStatus, parser.error()->code);
  ASSERT_TRUE(parser.status());
  EXPECT_EQ("first", parser.status()->motd);
}

TEST(JavaStatusStreamTest, FrameAndCumulativeInputHaveIndependentLimits) {
  JavaStatusStreamParser frame_parser(kCorrelation);
  auto frame = FrameForTest(std::vector<uint8_t>(kMaxSlpFrameBytes, 2));
  ASSERT_TRUE(frame_parser.Feed(frame));
  EXPECT_EQ(1u, frame_parser.ignored_packets());
  ASSERT_TRUE(frame_parser.Feed(StatusFrame(R"({"description":"ok"})")));
  ASSERT_TRUE(frame_parser.Feed(EncodePing(kCorrelation)));
  EXPECT_TRUE(frame_parser.complete());

  // Three-byte length prefix + body fits exactly half the total input limit.
  frame = FrameForTest(std::vector<uint8_t>(kMaxSlpFrameBytes - 3, 2));
  ASSERT_EQ(kMaxSlpInputBytes / 2, frame.size());
  JavaStatusStreamParser input_parser(kCorrelation);
  ASSERT_TRUE(input_parser.Feed(frame));
  ASSERT_TRUE(input_parser.Feed(frame));
  EXPECT_EQ(kMaxSlpInputBytes, input_parser.bytes_received());
  EXPECT_FALSE(input_parser.Feed(std::vector<uint8_t>{1}));
  ASSERT_TRUE(input_parser.error());
  EXPECT_EQ(SlpError::kInputLimit, input_parser.error()->code);
}

TEST(JavaStatusStreamTest, StatusSurvivesLaterBadPongAndTruncatedInput) {
  JavaStatusStreamParser parser(kCorrelation);
  ASSERT_TRUE(parser.Feed(StatusFrame(R"({"description":"ok"})")));
  EXPECT_FALSE(parser.Feed(FrameForTest({1, 0})));
  ASSERT_TRUE(parser.status());
  EXPECT_EQ("ok", parser.status()->motd);
  EXPECT_FALSE(parser.pong_received());
}

TEST(JavaStatusStreamTest, EndOfStreamRequiresStatusButNotPong) {
  JavaStatusStreamParser pong_only(kCorrelation);
  ASSERT_TRUE(pong_only.Feed(EncodePing(kCorrelation)));
  EXPECT_FALSE(pong_only.Finish());
  ASSERT_TRUE(pong_only.error());
  EXPECT_EQ(SlpError::kMissingStatus, pong_only.error()->code);

  JavaStatusStreamParser status_only(kCorrelation);
  ASSERT_TRUE(status_only.Feed(StatusFrame(R"({"description":"ok"})")));
  EXPECT_TRUE(status_only.Finish());
  EXPECT_FALSE(status_only.complete());
  EXPECT_FALSE(status_only.pong_received());
  EXPECT_TRUE(status_only.Finish());
  EXPECT_FALSE(status_only.Feed(EncodePing(kCorrelation)));
}

}  // namespace
}  // namespace mcje
