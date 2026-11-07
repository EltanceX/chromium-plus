// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCJE_JAVA_STATUS_JSON_H_
#define CHROME_BROWSER_MCJE_JAVA_STATUS_JSON_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "base/types/expected.h"

namespace mcje {

inline constexpr size_t kMaxStatusJsonBytes = 1024 * 1024;
inline constexpr size_t kMaxStatusJsonDepth = 64;
inline constexpr size_t kMaxTextDepth = 32;
inline constexpr size_t kMaxTextNodes = 1024;
inline constexpr size_t kMaxTextBytes = 16 * 1024;
inline constexpr size_t kMaxJsonSummaryBytes = 32 * 1024;
inline constexpr size_t kMaxFaviconDataUrlBytes =
    sizeof("data:image/png;base64,") - 1 + 4 * ((64 * 1024 + 2) / 3);

struct JavaServerStatus {
  // Plain UTF-8 text, not HTML. The result page must escape remote strings.
  std::string motd;
  std::optional<std::string> version_name;
  std::optional<int> protocol_version;
  std::optional<int> players_online;
  std::optional<int> players_max;

  // Untrusted, size-bounded attachment for JavaServerIconProcessor. Never pass
  // this string directly to the result page or an image element.
  std::optional<std::string> favicon;
  std::optional<std::string> favicon_error;
  std::string json_summary;
  bool json_summary_truncated = false;
};

base::expected<JavaServerStatus, std::string> ParseJavaStatusJson(
    std::string_view json);

}  // namespace mcje

#endif  // CHROME_BROWSER_MCJE_JAVA_STATUS_JSON_H_
