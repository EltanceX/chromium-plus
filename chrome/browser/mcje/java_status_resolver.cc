// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/java_status_resolver.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "net/base/net_errors.h"
#include "net/base/network_anonymization_key.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace mcje {

JavaStatusResolver::JavaStatusResolver() = default;

JavaStatusResolver::~JavaStatusResolver() {
  receiver_.reset();
  if (control_) {
    control_->Cancel(net::ERR_ABORTED);
  }
}

void JavaStatusResolver::Start(network::mojom::NetworkContext* context,
                               const net::HostPortPair& host,
                               net::DnsQueryType type,
                               Callback callback) {
  CHECK(!callback_);
  callback_ = std::move(callback);
  if (!context) {
    Complete(net::ERR_CONTEXT_SHUT_DOWN, {});
    return;
  }
  auto parameters = network::mojom::ResolveHostParameters::New();
  parameters->dns_query_type = type;
  parameters->control_handle = control_.BindNewPipeAndPassReceiver();
  auto client = receiver_.BindNewPipeAndPassRemote();
  receiver_.set_disconnect_handler(
      base::BindOnce(&JavaStatusResolver::Complete, base::Unretained(this),
                     net::ERR_FAILED, net::AddressList()));
  context->ResolveHost(network::mojom::HostResolverHost::NewHostPortPair(host),
                       net::NetworkAnonymizationKey::CreateTransient(),
                       std::move(parameters), std::move(client));
}

void JavaStatusResolver::OnHostnameResults(
    const std::vector<net::HostPortPair>& hosts) {
  // Bound work even if a resolver supplies an unexpectedly large result list.
  const size_t count = std::min(hosts.size(), size_t{16} - hosts_.size());
  hosts_.insert(hosts_.end(), hosts.begin(), hosts.begin() + count);
}

void JavaStatusResolver::OnComplete(int result,
                                    const net::ResolveErrorInfo&,
                                    const net::AddressList& addresses,
                                    const net::HostResolverEndpointResults&) {
  Complete(result, addresses);
}

void JavaStatusResolver::Complete(int error, net::AddressList addresses) {
  receiver_.reset();
  control_.reset();
  if (callback_) {
    std::move(callback_).Run({error, std::move(addresses), std::move(hosts_)});
  }
}

}  // namespace mcje
