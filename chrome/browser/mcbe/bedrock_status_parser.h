// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCBE_BEDROCK_STATUS_PARSER_H_
#define CHROME_BROWSER_MCBE_BEDROCK_STATUS_PARSER_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "base/types/expected.h"

namespace mcbe {

inline constexpr size_t kMaxPongSize = 8192;
inline constexpr std::array<uint8_t, 16> kRakNetMagic = {
    0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe, 0xfe,
    0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78};

struct BedrockStatus {
  std::string edition;
  std::string motd;
  int protocol = 0;
  std::string version;
  int players = 0;
  int max_players = 0;
  std::string server_guid;
  // The server ID advertised in MOTD can differ from the RakNet header GUID.
  std::string advertised_guid;
  std::string subtitle;
  std::string game_mode;
  std::optional<int> game_mode_id;
  std::optional<uint16_t> ipv4_port;
  std::optional<uint16_t> ipv6_port;
  std::string raw_motd;
};

std::array<uint8_t, 33> BuildUnconnectedPing(uint64_t marker,
                                             uint64_t client_guid);
base::expected<BedrockStatus, std::string> ParseUnconnectedPong(
    base::span<const uint8_t> packet,
    uint64_t expected_marker);
std::string StripMinecraftFormatting(std::string_view text);

}  // namespace mcbe

#endif  // CHROME_BROWSER_MCBE_BEDROCK_STATUS_PARSER_H_
