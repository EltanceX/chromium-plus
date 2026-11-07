// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCBE_BEDROCK_STATUS_QUERY_H_
#define CHROME_BROWSER_MCBE_BEDROCK_STATUS_QUERY_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/mcbe/bedrock_status_parser.h"
#include "chrome/browser/mcbe/mcbe_url.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/address_list.h"
#include "net/base/ip_endpoint.h"
#include "services/network/public/cpp/simple_host_resolver.h"
#include "services/network/public/mojom/udp_socket.mojom.h"

namespace mcbe {

enum class QueryError {
  kNone,
  kAddress,
  kDns,
  kNetwork,
  kTimeout,
  kInvalidResponse
};

struct QueryResult {
  QueryError error = QueryError::kNone;
  std::string message;
  std::optional<BedrockStatus> status;
  std::string endpoint;
  base::TimeDelta rtt;
  base::Time completed_at;
};

// Owned by a navigation loader. Destruction silently cancels all operations.
class BedrockStatusQuery : public network::mojom::UDPSocketListener {
 public:
  using Callback = base::OnceCallback<void(QueryResult)>;
  BedrockStatusQuery(network::SimpleHostResolver::NetworkContextFactory context,
                     ServerAddress address);
  ~BedrockStatusQuery() override;
  void Start(Callback callback);

 private:
  void OnResolved(int result,
                  const net::ResolveErrorInfo&,
                  const net::AddressList& addresses,
                  const net::HostResolverEndpointResults&);
  void TryEndpoint();
  void OnConnected(int attempt,
                   int result,
                   const std::optional<net::IPEndPoint>& local_address);
  void OnSent(int attempt, int result);
  void OnReceived(int32_t result,
                  const std::optional<net::IPEndPoint>& source,
                  std::optional<base::span<const uint8_t>> data) override;
  void OnNetworkError(int attempt, int error);
  void OnAttemptTimeout();
  void OnTimeout();
  void Finish(QueryResult result);

  network::SimpleHostResolver::NetworkContextFactory context_;
  const ServerAddress address_;
  Callback callback_;
  std::unique_ptr<network::SimpleHostResolver> resolver_;
  std::vector<net::IPEndPoint> endpoints_;
  int attempt_ = 0;
  int received_packets_ = 0;
  uint64_t marker_ = 0;
  base::TimeTicks sent_at_;
  base::TimeTicks deadline_;
  std::string invalid_response_;
  base::OneShotTimer total_timer_;
  base::OneShotTimer attempt_timer_;
  mojo::Remote<network::mojom::UDPSocket> socket_;
  mojo::Receiver<network::mojom::UDPSocketListener> listener_{this};
  base::WeakPtrFactory<BedrockStatusQuery> weak_factory_{this};
};

}  // namespace mcbe

#endif  // CHROME_BROWSER_MCBE_BEDROCK_STATUS_QUERY_H_
