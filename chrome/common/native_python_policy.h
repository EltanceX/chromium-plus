// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_COMMON_NATIVE_PYTHON_POLICY_H_
#define CHROME_COMMON_NATIVE_PYTHON_POLICY_H_

#include <optional>
#include <string_view>

#include "url/gurl.h"
#include "url/origin.h"

// The first experiment accepts one exact loopback HTTP(S) origin, without URL
// credentials, path, query, fragment, wildcards or a list of origins.
inline std::optional<url::Origin> ParseNativePythonAllowedOrigin(
    std::string_view value) {
  const GURL url(value);
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || url.has_username() ||
      url.has_password() || url.has_query() || url.has_ref() ||
      url.path() != "/" ||
      (url.host() != "127.0.0.1" && url.host() != "[::1]" &&
       url.host() != "localhost")) {
    return std::nullopt;
  }
  return url::Origin::Create(url);
}

#endif  // CHROME_COMMON_NATIVE_PYTHON_POLICY_H_
