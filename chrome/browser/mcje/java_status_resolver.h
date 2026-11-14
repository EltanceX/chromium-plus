// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCJE_JAVA_STATUS_RESOLVER_H_
#define CHROME_BROWSER_MCJE_JAVA_STATUS_RESOLVER_H_

#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/mcje/java_system_srv_lookup.h"
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
  bool used_system_srv = false;
};

// One request. Hostname results are provisional until OnComplete. Destruction
// cancels the control handle and closes the response pipe without a callback.
class JavaStatusResolver : public network::ResolveHostClientBase {
 public:
  using Callback = base::OnceCallback<void(JavaHostResolution)>;
  explicit JavaStatusResolver(JavaSystemSrvFactory system_srv_factory = {});
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
  void OnSystemSrv(int error, std::vector<net::HostPortPair> hosts);

  const JavaSystemSrvFactory system_srv_factory_;
  net::HostPortPair host_;
  net::DnsQueryType type_ = net::DnsQueryType::UNSPECIFIED;
  std::unique_ptr<JavaSystemSrvLookup> system_srv_;
  Callback callback_;
  std::vector<net::HostPortPair> hosts_;
  mojo::Receiver<network::mojom::ResolveHostClient> receiver_{this};
  mojo::Remote<network::mojom::ResolveHostHandle> control_;
  base::WeakPtrFactory<JavaStatusResolver> weak_factory_{this};
};

}  // namespace mcje

#endif  // CHROME_BROWSER_MCJE_JAVA_STATUS_RESOLVER_H_
