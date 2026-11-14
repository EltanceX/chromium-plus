// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/java_status_query.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/rand_util.h"
#include "net/base/net_errors.h"
#include "net/base/url_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace mcje {
namespace {

constexpr auto kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("mcje_java_status", R"(
      semantics {
        sender: "Minecraft Java status page"
        description:
          "A top-level mcje:// navigation optionally discovers a Minecraft "
          "SRV target, resolves its IP addresses and sends an uncompressed "
          "Java Server List Ping over TCP to display server status."
        trigger: "The user navigates to or refreshes an mcje:// URL."
        data: "Logical host and port, protocol version 47 and random Ping value. "
              "No account data, cookies or game login."
        destination: OTHER
        destination_other: "The URL server or its DNS SRV target."
      }
      policy {
        cookies_allowed: NO
        setting: "Only triggered by an mcje:// top-level navigation."
        policy_exception_justification:
          "Explicit navigation feature with no background polling."
      })");

}  // namespace

JavaStatusQuery::JavaStatusQuery(NetworkContextFactory context,
                                 ServerAddress address,
                                 JavaSystemSrvFactory system_srv_factory)
    : context_(std::move(context)),
      system_srv_factory_(std::move(system_srv_factory)),
      address_(std::move(address)),
      read_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL),
      write_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL) {}

JavaStatusQuery::~JavaStatusQuery() {
  Cancel();
}

void JavaStatusQuery::Start(Callback callback) {
  CHECK(!started_);
  started_ = true;
  callback_ = std::move(callback);
  auto handshake = EncodeHandshake(address_);
  if (!handshake.has_value()) {
    FinishError(JavaQueryError::kAddress, handshake.error());
    return;
  }
  outgoing_[0] = std::move(*handshake);
  outgoing_[1] = EncodeStatusRequest();
  if (!context_.Run()) {
    FinishError(JavaQueryError::kNetwork, "浏览器网络上下文已关闭。");
    return;
  }
  deadline_ = base::TimeTicks::Now() + base::Seconds(5);
  total_timer_.Start(
      FROM_HERE, base::Seconds(5),
      base::BindOnce(&JavaStatusQuery::OnTimeout, weak_factory_.GetWeakPtr()));
  if (address_.ShouldQuerySrv()) {
    phase_ = Phase::kSrv;
    phase_timer_.Start(FROM_HERE, base::Seconds(1),
                       base::BindOnce(&JavaStatusQuery::OnPhaseTimeout,
                                      weak_factory_.GetWeakPtr()));
    resolver_ = std::make_unique<JavaStatusResolver>(system_srv_factory_);
    resolver_->Start(
        context_.Run(),
        net::HostPortPair("_minecraft._tcp." + address_.logical_host, 0),
        net::DnsQueryType::SRV,
        base::BindOnce(&JavaStatusQuery::OnSrv, weak_factory_.GetWeakPtr()));
  } else {
    discovery_ =
        address_.is_ip_literal ? "IP 地址，跳过 SRV。" : "显式端口，跳过 SRV。";
    targets_.emplace_back(address_.logical_host, address_.logical_port);
    ResolveNextTarget();
  }
}

void JavaStatusQuery::Cancel() {
  callback_.Reset();
  weak_factory_.InvalidateWeakPtrs();
  total_timer_.Stop();
  phase_timer_.Stop();
  pong_timer_.Stop();
  resolver_.reset();
  CloseSocket();
}

void JavaStatusQuery::OnSrv(JavaHostResolution result) {
  phase_timer_.Stop();
  resolver_.reset();
  if (result.error == net::OK) {
    for (const auto& target : result.hosts) {
      if (target.port() && net::IsCanonicalizedHostCompliant(target.host()) &&
          std::ranges::find(targets_, target) == targets_.end()) {
        targets_.push_back(target);
        if (targets_.size() == 2) {
          break;
        }
      }
    }
  }
  if (targets_.empty()) {
    FallbackFromSrv("SRV 无可用结果，使用逻辑主机与默认端口：" +
                    net::ErrorToString(result.error));
    return;
  }
  using_srv_ = true;
  discovery_ = result.used_system_srv ? "使用系统 DNS SRV 的有序候选目标。"
                                      : "使用 DNS SRV 的有序候选目标。";
  ResolveNextTarget();
}

void JavaStatusQuery::FallbackFromSrv(std::string reason) {
  phase_timer_.Stop();
  resolver_.reset();
  discovery_ = std::move(reason);
  targets_.emplace_back(address_.logical_host, address_.logical_port);
  ResolveNextTarget();
}

void JavaStatusQuery::ResolveNextTarget() {
  if (base::TimeTicks::Now() >= deadline_) {
    OnTimeout();
    return;
  }
  if (attempts_ >= 2 || next_target_ >= targets_.size()) {
    FinishError(attempts_ ? JavaQueryError::kConnect : JavaQueryError::kDns,
                "没有剩余的可用连接端点。");
    return;
  }
  phase_ = Phase::kAddresses;
  if (!context_.Run()) {
    FinishError(JavaQueryError::kNetwork, "浏览器网络上下文已关闭。");
    return;
  }
  current_target_ = targets_[next_target_++];
  endpoints_.clear();
  next_endpoint_ = 0;
  if (next_target_ < targets_.size()) {
    phase_timer_.Start(
        FROM_HERE,
        std::min(base::Seconds(1), deadline_ - base::TimeTicks::Now()),
        base::BindOnce(&JavaStatusQuery::OnPhaseTimeout,
                       weak_factory_.GetWeakPtr()));
  }
  resolver_ = std::make_unique<JavaStatusResolver>();
  resolver_->Start(context_.Run(), current_target_,
                   net::DnsQueryType::UNSPECIFIED,
                   base::BindOnce(&JavaStatusQuery::OnAddresses,
                                  weak_factory_.GetWeakPtr()));
}

void JavaStatusQuery::OnAddresses(JavaHostResolution result) {
  phase_timer_.Stop();
  resolver_.reset();
  if (result.error != net::OK || result.addresses.empty()) {
    if (next_target_ < targets_.size()) {
      ResolveNextTarget();
      return;
    }
    FinishError(JavaQueryError::kDns,
                "DNS 地址解析失败：" + net::ErrorToString(result.error));
    return;
  }
  endpoints_.push_back(net::IPEndPoint(result.addresses.front().address(),
                                       current_target_.port()));
  auto alternative = std::ranges::find_if(result.addresses, [&](const auto& e) {
    return e.GetFamily() != endpoints_.front().GetFamily();
  });
  if (alternative == result.addresses.end()) {
    alternative = std::ranges::find_if(result.addresses, [&](const auto& e) {
      return e.address() != endpoints_.front().address();
    });
  }
  if (alternative != result.addresses.end()) {
    endpoints_.emplace_back(alternative->address(), current_target_.port());
  }
  TryEndpoint();
}

void JavaStatusQuery::TryEndpoint() {
  phase_timer_.Stop();
  CloseSocket();
  if (base::TimeTicks::Now() >= deadline_) {
    OnTimeout();
    return;
  }
  if (attempts_ >= 2) {
    FinishError(JavaQueryError::kConnect, "TCP 连接尝试次数已达到上限。");
    return;
  }
  if (next_endpoint_ >= endpoints_.size()) {
    ResolveNextTarget();
    return;
  }
  auto* context = context_.Run();
  if (!context) {
    FinishError(JavaQueryError::kNetwork, "浏览器网络上下文已关闭。");
    return;
  }
  phase_ = Phase::kConnect;
  const auto endpoint = endpoints_[next_endpoint_++];
  endpoint_ = endpoint.ToString();
  ++attempts_;
  const auto remaining = deadline_ - base::TimeTicks::Now();
  const bool has_alternative =
      attempts_ < 2 &&
      (next_endpoint_ < endpoints_.size() || next_target_ < targets_.size());
  phase_timer_.Start(FROM_HERE, has_alternative ? remaining / 2 : remaining,
                     base::BindOnce(&JavaStatusQuery::OnPhaseTimeout,
                                    weak_factory_.GetWeakPtr()));
  auto receiver = socket_.BindNewPipeAndPassReceiver();
  socket_.set_disconnect_handler(
      base::BindOnce(&JavaStatusQuery::OnSocketDisconnected,
                     weak_factory_.GetWeakPtr(), attempts_));
  auto observer = observer_.BindNewPipeAndPassRemote();
  observer_.set_disconnect_handler(
      base::BindOnce(&JavaStatusQuery::OnSocketDisconnected,
                     weak_factory_.GetWeakPtr(), attempts_));
  context->CreateTCPConnectedSocket(
      std::nullopt, net::AddressList(endpoint),
      network::mojom::TCPConnectedSocketOptions::New(),
      net::MutableNetworkTrafficAnnotationTag(kTrafficAnnotation),
      std::move(receiver), std::move(observer),
      base::BindOnce(&JavaStatusQuery::OnConnected, weak_factory_.GetWeakPtr(),
                     attempts_));
}

void JavaStatusQuery::OnConnected(int attempt,
                                  int result,
                                  const std::optional<net::IPEndPoint>& local,
                                  const std::optional<net::IPEndPoint>& peer,
                                  mojo::ScopedDataPipeConsumerHandle receive,
                                  mojo::ScopedDataPipeProducerHandle send) {
  if (attempt != attempts_ || phase_ != Phase::kConnect) {
    return;
  }
  if (result != net::OK || !receive.is_valid() || !send.is_valid()) {
    OnConnectionFailure(attempt, result == net::OK ? net::ERR_FAILED : result);
    return;
  }
  phase_timer_.Stop();
  phase_ = Phase::kRead;
  receive_ = std::move(receive);
  send_ = std::move(send);
  if (peer) {
    endpoint_ = peer->ToString();
  }
  const uint64_t marker = base::RandUint64();
  parser_ = std::make_unique<JavaStatusStreamParser>(marker);
  outgoing_[2] = EncodePing(marker);
  read_watcher_.Watch(
      receive_.get(),
      MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
      base::BindRepeating(&JavaStatusQuery::ReadMore,
                          weak_factory_.GetWeakPtr()));
  write_watcher_.Watch(
      send_.get(), MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
      base::BindRepeating(&JavaStatusQuery::WriteMore,
                          weak_factory_.GetWeakPtr()));
  auto weak = weak_factory_.GetWeakPtr();
  WriteMore();
  if (weak) {
    ReadMore();
  }
}

void JavaStatusQuery::OnConnectionFailure(int attempt, int error) {
  if (attempt != attempts_) {
    return;
  }
  if (attempts_ < 2 &&
      (next_endpoint_ < endpoints_.size() || next_target_ < targets_.size())) {
    TryEndpoint();
    return;
  }
  FinishError(JavaQueryError::kConnect,
              "TCP 连接失败：" + net::ErrorToString(error));
}

void JavaStatusQuery::WriteMore(MojoResult result) {
  while (write_packet_ < outgoing_.size()) {
    const auto bytes =
        base::span(outgoing_[write_packet_]).subspan(write_offset_);
    size_t written = 0;
    const auto submitted_at = base::TimeTicks::Now();
    result = send_->WriteData(bytes, MOJO_WRITE_DATA_FLAG_NONE, written);
    if (result == MOJO_RESULT_SHOULD_WAIT) {
      write_watcher_.ArmOrNotify();
      return;
    }
    if (result != MOJO_RESULT_OK) {
      OnWriteError(net::ERR_CONNECTION_CLOSED);
      return;
    }
    if (write_packet_ == 2 && write_offset_ == 0 && written) {
      ping_sent_at_ = submitted_at;
    }
    write_offset_ += written;
    if (write_offset_ == outgoing_[write_packet_].size()) {
      ++write_packet_;
      write_offset_ = 0;
    }
  }
  write_watcher_.Cancel();
}

void JavaStatusQuery::ReadMore(MojoResult result) {
  std::array<uint8_t, 16 * 1024> buffer;
  for (;;) {
    size_t count = 0;
    result = receive_->ReadData(MOJO_READ_DATA_FLAG_NONE, buffer, count);
    if (result == MOJO_RESULT_SHOULD_WAIT) {
      read_watcher_.ArmOrNotify();
      return;
    }
    if (result != MOJO_RESULT_OK) {
      const bool clean = parser_->Finish();
      const std::string detail =
          parser_->error() ? parser_->error()->message : "";
      if (!clean && !parser_->status()) {
        FinishError(read_error_.value_or(net::OK) < 0
                        ? JavaQueryError::kNetwork
                        : JavaQueryError::kProtocol,
                    "TCP 接收结束：" + detail);
      } else {
        CompleteResponse("连接已结束，未取得匹配 Pong。");
      }
      return;
    }
    const bool had_status = parser_->status().has_value();
    const bool had_pong = parser_->pong_received();
    const bool valid = parser_->Feed(base::span(buffer).first(count));
    if (!had_pong && parser_->pong_received() && !ping_sent_at_.is_null()) {
      rtt_ = base::TimeTicks::Now() - ping_sent_at_;
    }
    if (!valid) {
      if (parser_->status()) {
        CompleteResponse(parser_->error()->message);
      } else {
        FinishError(parser_->error()->code == SlpError::kInvalidJson
                        ? JavaQueryError::kJson
                        : JavaQueryError::kProtocol,
                    parser_->error()->message);
      }
      return;
    }
    if (parser_->complete()) {
      CompleteResponse("");
      return;
    }
    if (!had_status && parser_->status()) {
      pong_timer_.Start(
          FROM_HERE,
          std::min(base::Seconds(1), deadline_ - base::TimeTicks::Now()),
          base::BindOnce(&JavaStatusQuery::CompleteResponse,
                         weak_factory_.GetWeakPtr(),
                         "状态有效，等待 Pong 已超时。"));
    }
  }
}

void JavaStatusQuery::OnReadError(int error) {
  // The observer can precede readable notifications. Drain the receive pipe;
  // its producer closure, not observer ordering, decides when buffered data
  // ends.
  read_error_ = error;
  if (receive_.is_valid()) {
    ReadMore();
  }
}

void JavaStatusQuery::OnWriteError(int error) {
  if (receive_.is_valid()) {
    auto weak = weak_factory_.GetWeakPtr();
    ReadMore();
    if (!weak) {
      return;
    }
  }
  CompleteResponse("TCP 写入失败：" + net::ErrorToString(error));
}

void JavaStatusQuery::OnSocketDisconnected(int attempt) {
  if (attempt != attempts_) {
    return;
  }
  if (phase_ == Phase::kConnect) {
    OnConnectionFailure(attempt, net::ERR_CONNECTION_CLOSED);
  } else {
    // Drain data already handed to the consumer before completing. Service
    // loss also closes the producer, which wakes the watcher if it is empty.
    read_error_ = net::ERR_CONNECTION_CLOSED;
    ReadMore();
  }
}

void JavaStatusQuery::OnPhaseTimeout() {
  if (phase_ == Phase::kSrv) {
    FallbackFromSrv("SRV 超时，使用逻辑主机与默认端口。");
  } else if (phase_ == Phase::kAddresses) {
    resolver_.reset();
    ResolveNextTarget();
  } else if (phase_ == Phase::kConnect) {
    OnConnectionFailure(attempts_, net::ERR_TIMED_OUT);
  }
}

void JavaStatusQuery::OnTimeout() {
  if (parser_ && parser_->status()) {
    CompleteResponse("5 秒网络预算耗尽，未取得匹配 Pong。");
  } else {
    FinishError(JavaQueryError::kTimeout, "5 秒内未取得有效 Java 服务器状态。");
  }
}

void JavaStatusQuery::CompleteResponse(std::string reason) {
  if (!parser_ || !parser_->status()) {
    FinishError(JavaQueryError::kNetwork, std::move(reason));
    return;
  }
  JavaQueryResult result;
  result.status = *parser_->status();
  result.rtt = rtt_;
  if (!rtt_) {
    result.latency_diagnostic = std::move(reason);
  }
  Finish(std::move(result));
}

void JavaStatusQuery::FinishError(JavaQueryError error, std::string reason) {
  JavaQueryResult result;
  result.error = error;
  result.message = std::move(reason);
  Finish(std::move(result));
}

void JavaStatusQuery::Finish(JavaQueryResult result) {
  if (!callback_) {
    return;
  }
  result.endpoint = endpoint_;
  result.connection_attempts = attempts_;
  result.discovery = discovery_;
  if (using_srv_ && !current_target_.host().empty()) {
    result.srv_target = current_target_;
  }
  result.completed_at = base::Time::Now();
  auto callback = std::move(callback_);
  Cancel();
  std::move(callback).Run(std::move(result));
}

void JavaStatusQuery::CloseSocket() {
  read_watcher_.Cancel();
  write_watcher_.Cancel();
  receive_.reset();
  send_.reset();
  observer_.reset();
  socket_.reset();
  parser_.reset();
  read_error_.reset();
  rtt_.reset();
  ping_sent_at_ = {};
  write_packet_ = 0;
  write_offset_ = 0;
}

}  // namespace mcje
