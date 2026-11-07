// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcbe/bedrock_status_query.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/rand_util.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_errors.h"
#include "net/base/network_anonymization_key.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace mcbe {
namespace {

constexpr auto kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("mcbe_bedrock_status", R"(
      semantics {
        sender: "Minecraft Bedrock status page"
        description:
          "A top-level mcbe:// navigation resolves the selected host and sends "
          "up to two RakNet unconnected UDP pings to show server status."
        trigger: "The user navigates to or refreshes an mcbe:// URL."
        data: "A random request marker and random client GUID. No account data."
        destination: OTHER
        destination_other: "The server and UDP port selected in the URL."
      }
      policy {
        cookies_allowed: NO
        setting: "Triggered only by navigating to an mcbe:// result page."
        policy_exception_justification:
          "No background requests; this is an explicit navigation feature."
      })");

QueryResult Error(QueryError code, std::string message) {
  QueryResult result;
  result.error = code;
  result.message = std::move(message);
  return result;
}

}  // namespace

BedrockStatusQuery::BedrockStatusQuery(
    network::SimpleHostResolver::NetworkContextFactory context,
    ServerAddress address)
    : context_(std::move(context)), address_(std::move(address)) {}

BedrockStatusQuery::~BedrockStatusQuery() = default;

void BedrockStatusQuery::Start(Callback callback) {
  CHECK(!callback_);
  callback_ = std::move(callback);
  deadline_ = base::TimeTicks::Now() + base::Seconds(5);
  total_timer_.Start(FROM_HERE, base::Seconds(5), this,
                     &BedrockStatusQuery::OnTimeout);
  if (!context_.Run()) {
    Finish(Error(QueryError::kNetwork, "当前浏览器网络上下文已关闭。"));
    return;
  }
  resolver_ = network::SimpleHostResolver::Create(context_);
  resolver_->ResolveHost(network::mojom::HostResolverHost::NewHostPortPair(
                             net::HostPortPair(address_.host, address_.port)),
                         net::NetworkAnonymizationKey::CreateTransient(),
                         network::mojom::ResolveHostParameters::New(),
                         base::BindOnce(&BedrockStatusQuery::OnResolved,
                                        weak_factory_.GetWeakPtr()));
}

void BedrockStatusQuery::OnResolved(int result,
                                    const net::ResolveErrorInfo&,
                                    const net::AddressList& addresses,
                                    const net::HostResolverEndpointResults&) {
  if (result != net::OK || addresses.empty()) {
    Finish(
        Error(QueryError::kDns, "DNS 解析失败：" + net::ErrorToString(result)));
    return;
  }
  endpoints_.push_back(addresses.front());
  // Give the other address family a chance within the two-attempt budget.
  auto alternative = std::ranges::find_if(addresses, [&](const auto& endpoint) {
    return endpoint.GetFamily() != endpoints_.front().GetFamily();
  });
  if (alternative != addresses.end()) {
    endpoints_.push_back(*alternative);
  } else if (addresses.size() > 1) {
    endpoints_.push_back(addresses[1]);
  }
  TryEndpoint();
}

void BedrockStatusQuery::TryEndpoint() {
  attempt_timer_.Stop();
  socket_.reset();
  listener_.reset();
  if (base::TimeTicks::Now() >= deadline_) {
    OnTimeout();
    return;
  }
  ++attempt_;
  received_packets_ = 0;
  auto* context = context_.Run();
  if (!context) {
    Finish(Error(QueryError::kNetwork, "浏览器网络上下文已关闭。"));
    return;
  }
  context->CreateUDPSocket(socket_.BindNewPipeAndPassReceiver(),
                           listener_.BindNewPipeAndPassRemote());
  socket_.set_disconnect_handler(base::BindOnce(
      &BedrockStatusQuery::OnNetworkError, weak_factory_.GetWeakPtr(), attempt_,
      net::ERR_CONNECTION_CLOSED));
  listener_.set_disconnect_handler(base::BindOnce(
      &BedrockStatusQuery::OnNetworkError, weak_factory_.GetWeakPtr(), attempt_,
      net::ERR_CONNECTION_CLOSED));
  const base::TimeDelta remaining = deadline_ - base::TimeTicks::Now();
  attempt_timer_.Start(FROM_HERE, attempt_ == 1 ? remaining / 2 : remaining,
                       this, &BedrockStatusQuery::OnAttemptTimeout);
  const auto& endpoint = endpoints_[endpoints_.size() == 1 ? 0 : attempt_ - 1];
  socket_->Connect(endpoint, nullptr,
                   base::BindOnce(&BedrockStatusQuery::OnConnected,
                                  weak_factory_.GetWeakPtr(), attempt_));
}

void BedrockStatusQuery::OnConnected(
    int attempt,
    int result,
    const std::optional<net::IPEndPoint>& local_address) {
  if (attempt != attempt_) {
    return;
  }
  if (result != net::OK) {
    OnNetworkError(attempt, result);
    return;
  }
  marker_ = base::RandUint64();
  const auto ping = BuildUnconnectedPing(marker_, base::RandUint64());
  sent_at_ = base::TimeTicks::Now();
  socket_->ReceiveMoreWithBufferSize(1, kMaxPongSize);
  socket_->Send(ping,
                net::MutableNetworkTrafficAnnotationTag(kTrafficAnnotation),
                base::BindOnce(&BedrockStatusQuery::OnSent,
                               weak_factory_.GetWeakPtr(), attempt));
}

void BedrockStatusQuery::OnSent(int attempt, int result) {
  if (attempt == attempt_ && result != net::OK) {
    OnNetworkError(attempt, result);
  }
}

void BedrockStatusQuery::OnReceived(
    int32_t result,
    const std::optional<net::IPEndPoint>& source,
    std::optional<base::span<const uint8_t>> data) {
  if (result != net::OK) {
    OnNetworkError(attempt_, result);
    return;
  }
  auto parsed =
      ParseUnconnectedPong(data.value_or(base::span<const uint8_t>()), marker_);
  if (!parsed.has_value()) {
    invalid_response_ = parsed.error();
    // Ignore unrelated/malformed datagrams, but bound processing per attempt.
    if (++received_packets_ < 16) {
      socket_->ReceiveMoreWithBufferSize(1, kMaxPongSize);
    }
    return;
  }
  QueryResult response;
  response.status = std::move(parsed.value());
  response.rtt = base::TimeTicks::Now() - sent_at_;
  Finish(std::move(response));
}

void BedrockStatusQuery::OnNetworkError(int attempt, int error) {
  if (attempt != attempt_) {
    return;
  }
  if (attempt_ < 2) {
    TryEndpoint();
    return;
  }
  Finish(Error(QueryError::kNetwork,
               "UDP 网络查询失败：" + net::ErrorToString(error)));
}

void BedrockStatusQuery::OnAttemptTimeout() {
  if (attempt_ < 2) {
    TryEndpoint();
  } else {
    OnTimeout();
  }
}

void BedrockStatusQuery::OnTimeout() {
  if (!invalid_response_.empty()) {
    Finish(Error(QueryError::kInvalidResponse, invalid_response_));
    return;
  }
  Finish(Error(
      QueryError::kTimeout,
      "5 秒内未收到有效响应。服务器可能未开放此 UDP 端口，或网络未能送达。"));
}

void BedrockStatusQuery::Finish(QueryResult result) {
  CHECK(callback_);
  weak_factory_.InvalidateWeakPtrs();
  total_timer_.Stop();
  attempt_timer_.Stop();
  listener_.reset();
  socket_.reset();
  resolver_.reset();
  result.completed_at = base::Time::Now();
  if (attempt_ > 0 && !endpoints_.empty()) {
    result.endpoint =
        endpoints_[endpoints_.size() == 1 ? 0 : attempt_ - 1].ToString();
  }
  std::move(callback_).Run(std::move(result));
}

}  // namespace mcbe
