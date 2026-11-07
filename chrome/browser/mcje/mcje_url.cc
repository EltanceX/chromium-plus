// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/mcje_url.h"

#include "chrome/common/url_constants.h"
#include "url/url_constants.h"

namespace mcje {

base::expected<ServerAddress, std::string> ParseServerAddress(const GURL& url) {
  if (!url.is_valid() || !url.SchemeIs(chrome::kMcjeScheme) ||
      !url.has_host() || url.spec().size() > 2048) {
    return base::unexpected("地址无效，请使用 mcje://主机[:端口]/。");
  }
  if (url.has_username() || url.has_password() || url.has_query() ||
      url.has_ref() || (url.path() != "/" && !url.path().empty())) {
    return base::unexpected(
        "MCJE 地址仅支持主机和端口，不支持路径、查询、片段或用户信息。");
  }
  const int port = url.IntPort();
  if (port == 0 || port == url::PORT_INVALID) {
    return base::unexpected("TCP 端口必须在 1 到 65535 之间。");
  }
  ServerAddress address;
  address.logical_host = url.HostNoBrackets();
  if (address.logical_host.size() > 255) {
    return base::unexpected("Java 握手主机名不能超过 255 字节。");
  }
  address.port_was_explicit = port != url::PORT_UNSPECIFIED;
  if (address.port_was_explicit) {
    address.logical_port = static_cast<uint16_t>(port);
  }
  address.is_ip_literal = url.HostIsIPAddress();
  return address;
}

}  // namespace mcje
