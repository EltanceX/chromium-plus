// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcbe/mcbe_url.h"

#include "chrome/common/url_constants.h"
#include "url/url_constants.h"

namespace mcbe {

base::expected<ServerAddress, std::string> ParseServerAddress(const GURL& url) {
  if (!url.is_valid() || !url.SchemeIs(chrome::kMcbeScheme) ||
      !url.has_host() || url.spec().size() > 2048) {
    return base::unexpected("地址无效，请使用 mcbe://主机[:端口]/。");
  }
  if (url.has_username() || url.has_password() || url.has_query() ||
      url.has_ref() || (url.path() != "/" && !url.path().empty())) {
    return base::unexpected(
        "MCBE 地址仅支持主机和端口，不支持路径、查询、片段或用户信息。");
  }
  const int port = url.IntPort();
  if (port == 0 || port == url::PORT_INVALID) {
    return base::unexpected("UDP 端口必须在 1 到 65535 之间。");
  }
  ServerAddress address;
  address.host = url.HostNoBrackets();
  if (port != url::PORT_UNSPECIFIED) {
    address.port = static_cast<uint16_t>(port);
  }
  return address;
}

}  // namespace mcbe
