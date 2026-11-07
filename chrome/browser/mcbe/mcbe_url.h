// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCBE_MCBE_URL_H_
#define CHROME_BROWSER_MCBE_MCBE_URL_H_

#include <cstdint>
#include <string>

#include "base/types/expected.h"
#include "url/gurl.h"

namespace mcbe {

struct ServerAddress {
  std::string host;
  uint16_t port = 19132;
};

base::expected<ServerAddress, std::string> ParseServerAddress(const GURL& url);

}  // namespace mcbe

#endif  // CHROME_BROWSER_MCBE_MCBE_URL_H_
