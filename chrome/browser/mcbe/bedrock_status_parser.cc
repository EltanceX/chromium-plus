// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcbe/bedrock_status_parser.h"

#include <algorithm>
#include <string_view>

#include "base/numerics/byte_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"

namespace mcbe {
namespace {

bool ReadNonnegativeInt(std::string_view text, int* value) {
  return !text.empty() && std::ranges::all_of(text, base::IsAsciiDigit<char>) &&
         base::StringToInt(text, value) && *value >= 0;
}

}  // namespace

std::array<uint8_t, 33> BuildUnconnectedPing(uint64_t marker,
                                             uint64_t client_guid) {
  std::array<uint8_t, 33> packet = {};
  auto bytes = base::span(packet);
  bytes[0] = 0x01;
  bytes.subspan<1, 8>().copy_from(base::U64ToBigEndian(marker));
  bytes.subspan<9, 16>().copy_from(kRakNetMagic);
  bytes.subspan<25, 8>().copy_from(base::U64ToBigEndian(client_guid));
  return packet;
}

base::expected<BedrockStatus, std::string> ParseUnconnectedPong(
    base::span<const uint8_t> packet,
    uint64_t expected_marker) {
  if (packet.size() < 35 || packet.size() > kMaxPongSize) {
    return base::unexpected("响应包长度无效或超过 8 KiB 上限。");
  }
  if (packet[0] != 0x1c ||
      base::U64FromBigEndian(packet.subspan<1, 8>()) != expected_marker ||
      packet.subspan<17, 16>() != base::span(kRakNetMagic)) {
    return base::unexpected("响应包 ID、请求标记或 RakNet Magic 不匹配。");
  }
  const size_t length = base::U16FromBigEndian(packet.subspan<33, 2>());
  if (length != packet.size() - 35) {
    return base::unexpected("MOTD 声明长度与响应数据不一致。");
  }
  const std::string_view text = base::as_string_view(packet.subspan<35>());
  if (!base::IsStringUTF8(text) || text.find('\0') != std::string_view::npos) {
    return base::unexpected("MOTD 不是有效的 UTF-8 文本。");
  }
  auto fields = base::SplitStringPiece(text, ";", base::KEEP_WHITESPACE,
                                       base::SPLIT_WANT_ALL);
  BedrockStatus status;
  if (fields.size() < 6 || fields[0] != "MCPE" || fields[3].empty() ||
      !ReadNonnegativeInt(fields[2], &status.protocol) ||
      !ReadNonnegativeInt(fields[4], &status.players) ||
      !ReadNonnegativeInt(fields[5], &status.max_players)) {
    return base::unexpected("缺少 Bedrock MOTD 必要字段或数字超出范围。");
  }
  status.edition = fields[0];
  status.motd = StripMinecraftFormatting(fields[1]);
  status.version = fields[3];
  status.server_guid =
      base::NumberToString(base::U64FromBigEndian(packet.subspan<9, 8>()));
  if (fields.size() > 6 && !fields[6].empty()) {
    uint64_t guid = 0;
    if (!std::ranges::all_of(fields[6], base::IsAsciiDigit<char>) ||
        !base::StringToUint64(fields[6], &guid)) {
      return base::unexpected("MOTD 中的服务器 GUID 无效或超出 64 位范围。");
    }
    status.advertised_guid = base::NumberToString(guid);
  }
  if (fields.size() > 7) {
    status.subtitle = StripMinecraftFormatting(fields[7]);
  }
  if (fields.size() > 8) {
    status.game_mode = StripMinecraftFormatting(fields[8]);
  }
  if (fields.size() > 9 && !fields[9].empty()) {
    int mode = 0;
    if (!ReadNonnegativeInt(fields[9], &mode)) {
      return base::unexpected("游戏模式编号无效。");
    }
    status.game_mode_id = mode;
  }
  for (size_t i = 10; i < std::min(fields.size(), size_t{12}); ++i) {
    if (fields[i].empty()) {
      continue;
    }
    int port = 0;
    if (!ReadNonnegativeInt(fields[i], &port) || port < 1 || port > 65535) {
      return base::unexpected("服务器公布的端口超出范围。");
    }
    (i == 10 ? status.ipv4_port : status.ipv6_port) =
        static_cast<uint16_t>(port);
  }
  status.raw_motd = std::string(text);
  return status;
}

std::string StripMinecraftFormatting(std::string_view text) {
  std::string result;
  for (size_t i = 0; i < text.size();) {
    // The UTF-8 encoding of section sign is C2 A7. Consume only known codes.
    if (text.substr(i, 2) == "\xc2\xa7" && i + 2 < text.size() &&
        std::string_view("0123456789abcdefghijklmnopqrstu")
                .find(base::ToLowerASCII(text[i + 2])) !=
            std::string_view::npos) {
      i += 3;
    } else {
      result += text[i++];
    }
  }
  return result;
}

}  // namespace mcbe
