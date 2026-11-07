// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MINECRAFT_SERVER_STATUS_RESPONSE_H_
#define CHROME_BROWSER_MINECRAFT_SERVER_STATUS_RESPONSE_H_

#include <cstddef>
#include <string_view>

#include "services/network/public/mojom/url_loader.mojom-forward.h"

namespace minecraft {

inline constexpr size_t kMaxResultPageBytes = 512 * 1024;
enum class ResultPageImages { kNone, kData };

// Completes a bounded, local HTML response. The caller owns the client and
// resets it after this call. Images remain forbidden unless explicitly enabled
// by the protocol whose attachments have been validated and re-encoded.
void SendResultPage(network::mojom::URLLoaderClient* client,
                    std::string_view html,
                    ResultPageImages images);

}  // namespace minecraft

#endif  // CHROME_BROWSER_MINECRAFT_SERVER_STATUS_RESPONSE_H_
