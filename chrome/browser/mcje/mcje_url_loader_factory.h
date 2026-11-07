// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCJE_MCJE_URL_LOADER_FACTORY_H_
#define CHROME_BROWSER_MCJE_MCJE_URL_LOADER_FACTORY_H_

#include "content/public/browser/frame_tree_node_id.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/url_loader_factory.mojom-forward.h"

namespace mcje {

// Creates a browser-owned factory scoped to a top-level navigation frame.
mojo::PendingRemote<network::mojom::URLLoaderFactory> CreateNavigationFactory(
    content::FrameTreeNodeId frame_tree_node_id);

}  // namespace mcje

#endif  // CHROME_BROWSER_MCJE_MCJE_URL_LOADER_FACTORY_H_
