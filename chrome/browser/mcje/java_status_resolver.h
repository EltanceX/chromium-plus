// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCJE_JAVA_STATUS_RESOLVER_H_
#define CHROME_BROWSER_MCJE_JAVA_STATUS_RESOLVER_H_

#include <vector>

#include "base/functional/callback.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/address_list.h"
#include "net/base/host_port_pair.h"
#include "net/dns/public/dns_query_type.h"
#include "services/network/public/cpp/resolve_host_client_base.h"
#include "services/network/public/mojom/network_context.mojom-forward.h"

namespace mcje {

using NetworkContextFactory =
    base::RepeatingCallback<network::mojom::NetworkContext*()>;

struct JavaHostResolution {
  int error;
  net::AddressList addresses;
  std::vector<net::HostPortPair> hosts;
};

// One request. Hostname results are provisional until OnComplete. Destruction
// cancels the control handle and closes the response pipe without a callback.
class JavaStatusResolver : public network::ResolveHostClientBase {
 public:
  using Callback = base::OnceCallback<void(JavaHostResolution)>;
  JavaStatusResolver();
  ~JavaStatusResolver() override;
  void Start(network::mojom::NetworkContext* context,
             const net::HostPortPair& host,
             net::DnsQueryType type,
             Callback callback);

 private:
  void OnHostnameResults(const std::vector<net::HostPortPair>& hosts) override;
  void OnComplete(int result,
                  const net::ResolveErrorInfo&,
                  const net::AddressList& addresses,
                  const net::HostResolverEndpointResults&) override;
  void Complete(int error, net::AddressList addresses);

  Callback callback_;
  std::vector<net::HostPortPair> hosts_;
  mojo::Receiver<network::mojom::ResolveHostClient> receiver_{this};
  mojo::Remote<network::mojom::ResolveHostHandle> control_;
};

}  // namespace mcje

#endif  // CHROME_BROWSER_MCJE_JAVA_STATUS_RESOLVER_H_
