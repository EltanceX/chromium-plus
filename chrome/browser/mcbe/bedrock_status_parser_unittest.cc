// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcbe/bedrock_status_parser.h"

#include <vector>

#include "base/numerics/byte_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace mcbe {
namespace {

constexpr uint64_t kMarker = 0x0123456789abcdef;
constexpr uint64_t kGuid = 18446744073709551615ULL;

std::vector<uint8_t> Pong(std::string_view motd) {
  std::vector<uint8_t> packet(35 + motd.size());
  auto bytes = base::span(packet);
  bytes[0] = 0x1c;
  bytes.subspan<1, 8>().copy_from(base::U64ToBigEndian(kMarker));
  bytes.subspan<9, 8>().copy_from(base::U64ToBigEndian(kGuid));
  bytes.subspan<17, 16>().copy_from(kRakNetMagic);
  bytes.subspan<33, 2>().copy_from(base::U16ToBigEndian(motd.size()));
  bytes.subspan<35>().copy_from(base::as_byte_span(motd));
  return packet;
}

TEST(BedrockStatusParserTest, PingLayout) {
  const auto ping = BuildUnconnectedPing(kMarker, kGuid);
  const auto bytes = base::span(ping);
  EXPECT_EQ(0x01, bytes[0]);
  EXPECT_EQ(kMarker, base::U64FromBigEndian(bytes.subspan<1, 8>()));
  EXPECT_EQ(base::span(kRakNetMagic), (bytes.subspan<9, 16>()));
  EXPECT_EQ(kGuid, base::U64FromBigEndian(bytes.subspan<25, 8>()));
}

TEST(BedrockStatusParserTest, FullStatusAndUint64Precision) {
  auto result = ParseUnconnectedPong(
      Pong("MCPE;§a你好 <server>;944;1.26.14.1;12;100;18446744073709551615;"
           "§lSubtitle;Survival;1;19132;19133;extra;"),
      kMarker);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ("你好 <server>", result->motd);
  EXPECT_EQ("Subtitle", result->subtitle);
  EXPECT_EQ(12, result->players);
  EXPECT_EQ("18446744073709551615", result->server_guid);
  EXPECT_EQ("18446744073709551615", result->advertised_guid);
  EXPECT_EQ(19133, result->ipv6_port);
}

TEST(BedrockStatusParserTest, OptionalFieldsMayBeMissing) {
  auto result = ParseUnconnectedPong(Pong("MCPE;Title;1;v1;0;10"), kMarker);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ("18446744073709551615", result->server_guid);
  EXPECT_TRUE(result->subtitle.empty());
  EXPECT_TRUE(result->advertised_guid.empty());
  EXPECT_FALSE(result->ipv4_port);
}

TEST(BedrockStatusParserTest, ExtendedModeAndCanonicalGuid) {
  auto result = ParseUnconnectedPong(
      Pong("MCPE;Title;1;v1;0;10;018446744073709551615;;Spectator;6;"),
      kMarker);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(6, result->game_mode_id);
  EXPECT_EQ("18446744073709551615", result->server_guid);
  EXPECT_EQ("18446744073709551615", result->advertised_guid);
}

TEST(BedrockStatusParserTest, AcceptsIndependentMotdGuid) {
  auto result = ParseUnconnectedPong(Pong("MCPE;Title;1;v1;0;10;42"), kMarker);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ("18446744073709551615", result->server_guid);
  EXPECT_EQ("42", result->advertised_guid);
}

TEST(BedrockStatusParserTest, RejectsEveryTruncationAndWrongHeaders) {
  const auto packet = Pong("MCPE;Title;1;v1;0;10");
  for (size_t size = 0; size < packet.size(); ++size) {
    EXPECT_FALSE(ParseUnconnectedPong(base::span(packet).first(size), kMarker)
                     .has_value())
        << size;
  }
  for (size_t index :
       {size_t{0}, size_t{1}, size_t{17}, size_t{32}, size_t{34}}) {
    auto bad = packet;
    bad[index] ^= 1;
    EXPECT_FALSE(ParseUnconnectedPong(bad, kMarker).has_value()) << index;
  }
  EXPECT_FALSE(ParseUnconnectedPong(packet, kMarker + 1).has_value());
  auto extra = packet;
  extra.push_back(0);
  EXPECT_FALSE(ParseUnconnectedPong(extra, kMarker).has_value());
  EXPECT_FALSE(
      ParseUnconnectedPong(Pong(std::string(kMaxPongSize, 'a')), kMarker)
          .has_value());
}

TEST(BedrockStatusParserTest, RejectsInvalidFields) {
  for (const auto* text :
       {"OTHER;Title;1;v;0;10", "MCPE;Title;1;v;0", "MCPE;Title;1;;0;10",
        "MCPE;Title;-1;v;0;10", "MCPE;Title;1;v;-1;10",
        "MCPE;Title;1;v;0;2147483648", "MCPE;Title;1;v;0; 10",
        "MCPE;Title;1;v;0;10;abc", "MCPE;Title;1;v;0;10;18446744073709551616",
        "MCPE;Title;1;v;0;10;;;Mode;abc", "MCPE;Title;1;v;0;10;;;Mode;1;0",
        "MCPE;Title;1;v;0;10;;;Mode;1;65536", "MCPE;\xff;1;v;0;10"}) {
    EXPECT_FALSE(ParseUnconnectedPong(Pong(text), kMarker).has_value()) << text;
  }
  const std::string null_text("MCPE;A\0B;1;v;0;10", 18);
  EXPECT_FALSE(ParseUnconnectedPong(Pong(null_text), kMarker).has_value());
}

TEST(BedrockStatusParserTest, FormattingPreservesUnrecognizedText) {
  EXPECT_EQ("Hello 世界", StripMinecraftFormatting("§aHello §r世界"));
  EXPECT_EQ("§z§", StripMinecraftFormatting("§z§"));
}

}  // namespace
}  // namespace mcbe
