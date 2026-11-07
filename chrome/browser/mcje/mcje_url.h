// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCJE_MCJE_URL_H_
#define CHROME_BROWSER_MCJE_MCJE_URL_H_

#include <cstdint>
#include <string>

#include "base/types/expected.h"
#include "url/gurl.h"

namespace mcje {

inline constexpr uint16_t kDefaultJavaPort = 25565;

// The logical handshake target, independent of any future SRV/DNS endpoint.
struct ServerAddress {
  std::string logical_host;
  uint16_t logical_port = kDefaultJavaPort;
  bool port_was_explicit = false;
  bool is_ip_literal = false;

  bool ShouldQuerySrv() const { return !port_was_explicit && !is_ip_literal; }
};

base::expected<ServerAddress, std::string> ParseServerAddress(const GURL& url);

}  // namespace mcje

#endif  // CHROME_BROWSER_MCJE_MCJE_URL_H_
