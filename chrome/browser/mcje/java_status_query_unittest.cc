// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/java_status_query.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "net/base/net_errors.h"
#include "net/base/network_anonymization_key.h"
#include "net/dns/public/resolve_error_info.h"
#include "services/network/test/test_network_context.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace mcje {
namespace {

class FakeResolution : public network::mojom::ResolveHostHandle {
 public:
  FakeResolution(
      net::HostPortPair name,
      net::DnsQueryType type,
      mojo::PendingReceiver<network::mojom::ResolveHostHandle> control,
      mojo::PendingRemote<network::mojom::ResolveHostClient> client)
      : host(std::move(name)),
        type(type),
        client(std::move(client)),
        receiver(this, std::move(control)) {}
  void Cancel(int error) override { cancelled = true; }
  net::HostPortPair host;
  net::DnsQueryType type;
  mojo::Remote<network::mojom::ResolveHostClient> client;
  mojo::Receiver<network::mojom::ResolveHostHandle> receiver;
  bool cancelled = false;
};

class FakeNetwork : public network::TestNetworkContext {
 public:
  struct Connection {
    net::IPEndPoint endpoint;
    mojo::ScopedMessagePipeHandle socket;
    mojo::Remote<network::mojom::SocketObserver> observer;
    CreateTCPConnectedSocketCallback callback;
    mojo::ScopedDataPipeProducerHandle response;
    mojo::ScopedDataPipeConsumerHandle request;
  };

  void ResolveHost(
      network::mojom::HostResolverHostPtr host,
      const net::NetworkAnonymizationKey& key,
      network::mojom::ResolveHostParametersPtr parameters,
      mojo::PendingRemote<network::mojom::ResolveHostClient> client) override {
    EXPECT_TRUE(key.IsTransient());
    resolutions.push_back(std::make_unique<FakeResolution>(
        host->get_host_port_pair(), parameters->dns_query_type,
        std::move(parameters->control_handle), std::move(client)));
  }

  void CreateTCPConnectedSocket(
      const std::optional<net::IPEndPoint>& local,
      const net::AddressList& addresses,
      network::mojom::TCPConnectedSocketOptionsPtr options,
      const net::MutableNetworkTrafficAnnotationTag& annotation,
      mojo::PendingReceiver<network::mojom::TCPConnectedSocket> socket,
      mojo::PendingRemote<network::mojom::SocketObserver> observer,
      CreateTCPConnectedSocketCallback callback) override {
    ASSERT_EQ(1u, addresses.size());
    auto connection = std::make_unique<Connection>();
    connection->endpoint = addresses.front();
    connection->socket = socket.PassPipe();
    connection->observer.Bind(std::move(observer));
    connection->callback = std::move(callback);
    connections.push_back(std::move(connection));
  }

  void CompleteConnect(size_t index, int error = net::OK) {
    auto& c = *connections[index];
    mojo::ScopedDataPipeConsumerHandle receive;
    mojo::ScopedDataPipeProducerHandle send;
    if (error == net::OK) {
      CHECK_EQ(MOJO_RESULT_OK,
               mojo::CreateDataPipe(65536, c.response, receive));
      // Force partial writes and writable notifications in the real query.
      CHECK_EQ(MOJO_RESULT_OK, mojo::CreateDataPipe(8, send, c.request));
    }
    std::move(c.callback)
        .Run(error, std::nullopt, c.endpoint, std::move(receive),
             std::move(send));
  }

  std::vector<std::unique_ptr<FakeResolution>> resolutions;
  std::vector<std::unique_ptr<Connection>> connections;
};

std::vector<uint8_t> StatusFrame(std::string_view json) {
  std::vector<uint8_t> packet{0};
  auto length = EncodeVarInt(static_cast<int32_t>(json.size()));
  packet.insert(packet.end(), length.begin(), length.end());
  packet.insert(packet.end(), json.begin(), json.end());
  auto frame = EncodeVarInt(static_cast<int32_t>(packet.size()));
  frame.insert(frame.end(), packet.begin(), packet.end());
  return frame;
}

struct FakeSystemSrvState {
  std::string name;
  JavaSystemSrvLookup::Callback callback;
  int starts = 0;
  int cancels = 0;
  bool synchronous = false;
};

class FakeSystemSrvLookup : public JavaSystemSrvLookup {
 public:
  explicit FakeSystemSrvLookup(std::shared_ptr<FakeSystemSrvState> state)
      : state_(std::move(state)) {}
  void Start(std::string name, Callback callback) override {
    auto state = state_;
    state->name = std::move(name);
    ++state->starts;
    state->callback = std::move(callback);
    if (state->synchronous) {
      std::move(state->callback)
          .Run(net::OK, {net::HostPortPair("system.test", 11964)});
    }
  }
  void Cancel() override {
    ++state_->cancels;
    // Deliberately retain the callback to exercise late OS completion.
  }

 private:
  const std::shared_ptr<FakeSystemSrvState> state_;
};

class JavaStatusQueryTest : public testing::Test {
 protected:
  void EnableSystemSrv() {
    system_srv_factory = base::BindRepeating(
        [](std::shared_ptr<FakeSystemSrvState> state)
            -> std::unique_ptr<JavaSystemSrvLookup> {
          return std::make_unique<FakeSystemSrvLookup>(std::move(state));
        },
        system_srv);
  }

  void Start(bool explicit_port = false, std::string host = "logical.test") {
    address.logical_host = std::move(host);
    address.port_was_explicit = explicit_port;
    net::IPAddress ip;
    address.is_ip_literal = ip.AssignFromIPLiteral(address.logical_host);
    query = std::make_unique<JavaStatusQuery>(
        base::BindRepeating(
            [](JavaStatusQueryTest* self) -> network::mojom::NetworkContext* {
              return self->context_alive ? &self->network : nullptr;
            },
            base::Unretained(this)),
        address, system_srv_factory);
    query->Start(base::BindOnce(
        [](JavaStatusQueryTest* self, JavaQueryResult value) {
          ++self->callbacks;
          self->result = std::move(value);
          if (self->destroy_on_callback) {
            self->query.reset();
          }
        },
        base::Unretained(this)));
    tasks.RunUntilIdle();
  }

  void Reply(size_t index,
             int error,
             net::AddressList addresses = {},
             std::vector<net::HostPortPair> hosts = {},
             std::optional<int> detailed_error = std::nullopt) {
    auto& r = *network.resolutions[index];
    if (!hosts.empty()) {
      r.client->OnHostnameResults(hosts);
    }
    r.client->OnComplete(error,
                         net::ResolveErrorInfo(detailed_error.value_or(error)),
                         addresses, {});
    tasks.RunUntilIdle();
  }

  net::AddressList Addresses() {
    return net::AddressList(
        net::IPEndPoint(net::IPAddress::IPv4Localhost(), 25565));
  }

  std::vector<uint8_t> Connect(size_t index = 0) {
    network.CompleteConnect(index);
    std::vector<uint8_t> bytes;
    auto& c = *network.connections[index];
    for (int i = 0; i < 100; ++i) {
      tasks.RunUntilIdle();
      std::array<uint8_t, 64> buffer;
      size_t count = 0;
      const auto read =
          c.request->ReadData(MOJO_READ_DATA_FLAG_NONE, buffer, count);
      if (read == MOJO_RESULT_SHOULD_WAIT) {
        break;
      }
      CHECK_EQ(MOJO_RESULT_OK, read);
      bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
    }
    tasks.RunUntilIdle();
    return bytes;
  }

  void Send(base::span<const uint8_t> bytes,
            size_t index = 0,
            bool flush = true) {
    ASSERT_EQ(MOJO_RESULT_OK,
              network.connections[index]->response->WriteAllData(bytes));
    if (flush) {
      tasks.RunUntilIdle();
    }
  }

  void ConnectDirect() {
    Start(true);
    Reply(0, net::OK, Addresses());
    request = Connect();
    ASSERT_GE(request.size(), 10u);
  }

  void Pong(size_t index = 0) { Send(base::span(request).last<10>(), index); }

  base::test::TaskEnvironment tasks{
      base::test::TaskEnvironment::MainThreadType::IO,
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  FakeNetwork network;
  const std::shared_ptr<FakeSystemSrvState> system_srv =
      std::make_shared<FakeSystemSrvState>();
  JavaSystemSrvFactory system_srv_factory;
  ServerAddress address;
  std::unique_ptr<JavaStatusQuery> query;
  std::optional<JavaQueryResult> result;
  std::vector<uint8_t> request;
  int callbacks = 0;
  bool context_alive = true;
  bool destroy_on_callback = false;
};

TEST_F(JavaStatusQueryTest, SrvWaitsForCompletionAndPreservesLogicalHandshake) {
  EnableSystemSrv();
  Start();
  ASSERT_EQ(1u, network.resolutions.size());
  EXPECT_EQ(net::DnsQueryType::SRV, network.resolutions[0]->type);
  EXPECT_EQ("_minecraft._tcp.logical.test",
            network.resolutions[0]->host.host());
  network.resolutions[0]->client->OnHostnameResults(
      {net::HostPortPair("backend.test", 25570)});
  tasks.RunUntilIdle();
  EXPECT_EQ(1u, network.resolutions.size());
  EXPECT_TRUE(network.connections.empty());
  Reply(0, net::OK);  // Empty AddressList is correct for an SRV response.
  ASSERT_EQ(2u, network.resolutions.size());
  EXPECT_EQ(net::HostPortPair("backend.test", 25570),
            network.resolutions[1]->host);
  Reply(1, net::OK, Addresses());
  ASSERT_EQ(1u, network.connections.size());
  EXPECT_EQ(25570, network.connections[0]->endpoint.port());
  request = Connect();
  auto handshake = EncodeHandshake(address);
  ASSERT_TRUE(handshake.has_value());
  ASSERT_GE(request.size(), handshake->size() + 12);
  EXPECT_EQ(*handshake,
            std::vector<uint8_t>(request.begin(),
                                 request.begin() + handshake->size()));
  EXPECT_EQ(1, request[handshake->size()]);
  EXPECT_EQ(0, request[handshake->size() + 1]);
  Pong();
  EXPECT_FALSE(result);
  Send(StatusFrame(R"({"description":"SRV success"})"));
  ASSERT_TRUE(result && result->status);
  EXPECT_EQ("SRV success", result->status->motd);
  EXPECT_EQ(net::HostPortPair("backend.test", 25570), result->srv_target);
  EXPECT_TRUE(result->rtt);
  EXPECT_EQ(1, callbacks);
  EXPECT_EQ(0, system_srv->starts);
}

TEST_F(JavaStatusQueryTest,
       MissingDnsCapabilityUsesSystemSrvAndLogicalHandshake) {
  EnableSystemSrv();
  Start();
  Reply(0, net::ERR_NAME_NOT_RESOLVED, {}, {}, net::ERR_DNS_CACHE_MISS);
  ASSERT_EQ(1, system_srv->starts);
  EXPECT_EQ("_minecraft._tcp.logical.test", system_srv->name);
  EXPECT_EQ(1u, network.resolutions.size());
  std::move(system_srv->callback)
      .Run(net::OK, {net::HostPortPair("system.test", 11964)});
  tasks.RunUntilIdle();
  ASSERT_EQ(2u, network.resolutions.size());
  EXPECT_EQ(net::HostPortPair("system.test", 11964),
            network.resolutions[1]->host);
  Reply(1, net::OK, Addresses());
  ASSERT_EQ(1u, network.connections.size());
  EXPECT_EQ(11964, network.connections[0]->endpoint.port());
  request = Connect();
  auto handshake = EncodeHandshake(address);
  ASSERT_TRUE(handshake);
  ASSERT_GE(request.size(), handshake->size());
  EXPECT_EQ(*handshake,
            std::vector<uint8_t>(request.begin(),
                                 request.begin() + handshake->size()));
  Send(StatusFrame(R"({"description":"system SRV success"})"));
  Pong();
  ASSERT_TRUE(result && result->status);
  EXPECT_EQ("system SRV success", result->status->motd);
  EXPECT_EQ(net::HostPortPair("system.test", 11964), result->srv_target);
  EXPECT_NE(std::string::npos, result->discovery.find("系统 DNS SRV"));
  EXPECT_EQ(1, callbacks);
}

TEST_F(JavaStatusQueryTest, NegativeDnsResponseDoesNotUseSystemSrv) {
  EnableSystemSrv();
  Start();
  Reply(0, net::ERR_NAME_NOT_RESOLVED);
  EXPECT_EQ(0, system_srv->starts);
  ASSERT_EQ(2u, network.resolutions.size());
  EXPECT_EQ("logical.test", network.resolutions[1]->host.host());
}

TEST_F(JavaStatusQueryTest, PolicyCanDeclineSystemSrv) {
  int factory_calls = 0;
  system_srv_factory = base::BindRepeating(
      [](int* calls) -> std::unique_ptr<JavaSystemSrvLookup> {
        ++*calls;
        return nullptr;
      },
      &factory_calls);
  Start();
  Reply(0, net::ERR_NAME_NOT_RESOLVED, {}, {}, net::ERR_DNS_CACHE_MISS);
  EXPECT_EQ(1, factory_calls);
  EXPECT_EQ(0, system_srv->starts);
  ASSERT_EQ(2u, network.resolutions.size());
  EXPECT_EQ("logical.test", network.resolutions[1]->host.host());
}

TEST_F(JavaStatusQueryTest, SystemSrvFailurePreservesTotalDeadline) {
  EnableSystemSrv();
  Start();
  tasks.FastForwardBy(base::Milliseconds(200));
  Reply(0, net::ERR_NAME_NOT_RESOLVED, {}, {}, net::ERR_DNS_CACHE_MISS);
  tasks.FastForwardBy(base::Milliseconds(600));
  std::move(system_srv->callback).Run(net::ERR_NAME_NOT_RESOLVED, {});
  ASSERT_EQ(2u, network.resolutions.size());
  EXPECT_EQ("logical.test", network.resolutions[1]->host.host());
  tasks.FastForwardBy(base::Milliseconds(4200));
  ASSERT_TRUE(result);
  EXPECT_EQ(JavaQueryError::kTimeout, result->error);
  EXPECT_EQ(1, callbacks);
}

TEST_F(JavaStatusQueryTest,
       SystemSrvSharesPhaseTimeoutAndIgnoresLateCompletion) {
  EnableSystemSrv();
  Start();
  tasks.FastForwardBy(base::Milliseconds(800));
  Reply(0, net::ERR_NAME_NOT_RESOLVED, {}, {}, net::ERR_DNS_CACHE_MISS);
  ASSERT_EQ(1, system_srv->starts);
  tasks.FastForwardBy(base::Milliseconds(200));
  EXPECT_EQ(1, system_srv->cancels);
  ASSERT_EQ(2u, network.resolutions.size());
  EXPECT_EQ("logical.test", network.resolutions[1]->host.host());
  std::move(system_srv->callback)
      .Run(net::OK, {net::HostPortPair("late.test", 11964)});
  tasks.RunUntilIdle();
  EXPECT_EQ(2u, network.resolutions.size());
  EXPECT_EQ(0, callbacks);
}

TEST_F(JavaStatusQueryTest, CancelDuringSystemSrvDiscardsLateCompletion) {
  EnableSystemSrv();
  Start();
  Reply(0, net::ERR_NAME_NOT_RESOLVED, {}, {}, net::ERR_DNS_CACHE_MISS);
  ASSERT_EQ(1, system_srv->starts);
  query.reset();
  EXPECT_EQ(1, system_srv->cancels);
  std::move(system_srv->callback)
      .Run(net::OK, {net::HostPortPair("late.test", 11964)});
  tasks.FastForwardBy(base::Seconds(10));
  EXPECT_EQ(1u, network.resolutions.size());
  EXPECT_TRUE(network.connections.empty());
  EXPECT_EQ(0, callbacks);
}

TEST_F(JavaStatusQueryTest, SynchronousSystemSrvCompletionCanDestroyResolver) {
  EnableSystemSrv();
  system_srv->synchronous = true;
  Start();
  Reply(0, net::ERR_NAME_NOT_RESOLVED, {}, {}, net::ERR_DNS_CACHE_MISS);
  EXPECT_EQ(1, system_srv->starts);
  EXPECT_EQ(1, system_srv->cancels);
  ASSERT_EQ(2u, network.resolutions.size());
  EXPECT_EQ(net::HostPortPair("system.test", 11964),
            network.resolutions[1]->host);
}

TEST_F(JavaStatusQueryTest, ExplicitDefaultPortSkipsSrv) {
  EnableSystemSrv();
  Start(true);
  ASSERT_EQ(1u, network.resolutions.size());
  EXPECT_EQ(net::DnsQueryType::UNSPECIFIED, network.resolutions[0]->type);
  EXPECT_EQ(net::HostPortPair("logical.test", 25565),
            network.resolutions[0]->host);
  Reply(0, net::ERR_NAME_NOT_RESOLVED, {}, {}, net::ERR_DNS_CACHE_MISS);
  EXPECT_EQ(0, system_srv->starts);
}

TEST_F(JavaStatusQueryTest, IpLiteralSkipsSrv) {
  Start(false, "::1");
  ASSERT_EQ(1u, network.resolutions.size());
  EXPECT_EQ(net::DnsQueryType::UNSPECIFIED, network.resolutions[0]->type);
}

TEST_F(JavaStatusQueryTest, SrvFailureFallsBackWithoutResettingTotalBudget) {
  Start();
  tasks.FastForwardBy(base::Milliseconds(800));
  Reply(0, net::ERR_NAME_NOT_RESOLVED);
  ASSERT_EQ(2u, network.resolutions.size());
  EXPECT_EQ(net::HostPortPair("logical.test", 25565),
            network.resolutions[1]->host);
  tasks.FastForwardBy(base::Milliseconds(4200));
  ASSERT_TRUE(result);
  EXPECT_EQ(JavaQueryError::kTimeout, result->error);
  EXPECT_EQ(1, callbacks);
}

TEST_F(JavaStatusQueryTest, SrvTimeoutCancelsAndIgnoresLateResults) {
  Start();
  tasks.FastForwardBy(base::Seconds(1));
  ASSERT_EQ(2u, network.resolutions.size());
  EXPECT_TRUE(network.resolutions[0]->cancelled);
  Reply(0, net::OK, {}, {net::HostPortPair("late.test", 42)});
  EXPECT_EQ(2u, network.resolutions.size());
  EXPECT_EQ("logical.test", network.resolutions[1]->host.host());
}

TEST_F(JavaStatusQueryTest, InvalidSrvTargetsAreFilteredAndCandidatesBounded) {
  Start();
  Reply(0, net::OK, {},
        {net::HostPortPair(".", 1), net::HostPortPair("bad/path", 1),
         net::HostPortPair("bad.test", 0), net::HostPortPair("one.test", 1),
         net::HostPortPair("one.test", 1), net::HostPortPair("two.test", 2),
         net::HostPortPair("three.test", 3)});
  ASSERT_EQ(2u, network.resolutions.size());
  EXPECT_EQ("one.test", network.resolutions[1]->host.host());
  Reply(1, net::ERR_NAME_NOT_RESOLVED);
  ASSERT_EQ(3u, network.resolutions.size());
  EXPECT_EQ("two.test", network.resolutions[2]->host.host());
  Reply(2, net::ERR_NAME_NOT_RESOLVED);
  ASSERT_TRUE(result);
  EXPECT_EQ(JavaQueryError::kDns, result->error);
  EXPECT_EQ(3u, network.resolutions.size());
}

TEST_F(JavaStatusQueryTest, SelectedSrvFailureDoesNotReconnectOriginalHost) {
  Start();
  Reply(0, net::OK, {}, {net::HostPortPair("backend.test", 25570)});
  Reply(1, net::OK, Addresses());
  network.CompleteConnect(0, net::ERR_CONNECTION_REFUSED);
  tasks.RunUntilIdle();
  ASSERT_TRUE(result);
  EXPECT_EQ(JavaQueryError::kConnect, result->error);
  EXPECT_EQ(1, result->connection_attempts);
  EXPECT_EQ(2u, network.resolutions.size());
  EXPECT_EQ(1u, network.connections.size());
}

TEST_F(JavaStatusQueryTest,
       ConnectionTimeoutTriesOtherFamilyWithinTwoAttempts) {
  Start(true);
  const auto v4 = net::IPAddress::IPv4Localhost();
  const auto v6 = net::IPAddress::IPv6Localhost();
  Reply(0, net::OK,
        net::AddressList(
            std::vector<net::IPEndPoint>{{v4, 1}, {v4, 1}, {v6, 1}}));
  ASSERT_EQ(1u, network.connections.size());
  tasks.FastForwardBy(base::Milliseconds(2500));
  ASSERT_EQ(2u, network.connections.size());
  EXPECT_EQ(v6, network.connections[1]->endpoint.address());
  network.CompleteConnect(0);  // Stale success is ignored and its pipes close.
  tasks.RunUntilIdle();
  EXPECT_FALSE(result);
  request = Connect(1);
  Send(StatusFrame(R"({"description":"second"})"), 1);
  Pong(1);
  ASSERT_TRUE(result && result->status);
  EXPECT_EQ(2, result->connection_attempts);
  EXPECT_TRUE(
      network.connections[0]->response->QuerySignalsState().peer_closed());
}

TEST_F(JavaStatusQueryTest, TwoFailedEndpointsDoNotRetryAgain) {
  Start(true);
  Reply(0, net::OK,
        net::AddressList(std::vector<net::IPEndPoint>{
            {net::IPAddress::IPv4Localhost(), 1},
            {net::IPAddress::IPv6Localhost(), 1}}));
  network.CompleteConnect(0, net::ERR_CONNECTION_REFUSED);
  network.CompleteConnect(1, net::ERR_CONNECTION_REFUSED);
  tasks.RunUntilIdle();
  ASSERT_TRUE(result);
  EXPECT_EQ(2, result->connection_attempts);
  EXPECT_EQ(2u, network.connections.size());
  EXPECT_EQ(JavaQueryError::kConnect, result->error);
}

TEST_F(JavaStatusQueryTest, StatusWithoutPongSucceedsAfterOneSecond) {
  ConnectDirect();
  const auto start = base::TimeTicks::Now();
  auto frame = StatusFrame(R"({"description":"kept"})");
  Send(base::span(frame).first<3>());
  EXPECT_FALSE(result);
  Send(base::span(frame).subspan<3>());
  tasks.FastForwardBy(base::Seconds(1));
  ASSERT_TRUE(result && result->status);
  EXPECT_EQ(JavaQueryError::kNone, result->error);
  EXPECT_EQ("kept", result->status->motd);
  EXPECT_FALSE(result->rtt);
  EXPECT_FALSE(result->latency_diagnostic.empty());
  EXPECT_EQ(base::Seconds(1), base::TimeTicks::Now() - start);
  EXPECT_EQ(1, callbacks);
}

TEST_F(JavaStatusQueryTest,
       BufferedStatusIsDrainedWhenObserverAndEofArriveTogether) {
  ConnectDirect();
  destroy_on_callback = true;
  Send(StatusFrame(R"({"description":"buffered"})"), 0, false);
  network.connections[0]->observer->OnReadError(net::OK);
  network.connections[0]->response.reset();
  tasks.RunUntilIdle();
  ASSERT_TRUE(result && result->status);
  EXPECT_EQ("buffered", result->status->motd);
  EXPECT_FALSE(query);
  EXPECT_EQ(1, callbacks);
}

TEST_F(JavaStatusQueryTest, MalformedJsonHasJsonErrorAndNoRetry) {
  ConnectDirect();
  Send(StatusFrame(R"({"description":false})"));
  ASSERT_TRUE(result);
  EXPECT_EQ(JavaQueryError::kJson, result->error);
  EXPECT_EQ(1u, network.connections.size());
}

TEST_F(JavaStatusQueryTest, PongOnlyDoesNotBecomeSuccessfulStatus) {
  ConnectDirect();
  Pong();
  network.connections[0]->response.reset();
  tasks.RunUntilIdle();
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->status);
  EXPECT_EQ(JavaQueryError::kProtocol, result->error);
}

TEST_F(JavaStatusQueryTest, ReadTimeoutSharesFiveSecondBudget) {
  ConnectDirect();
  tasks.FastForwardBy(base::Seconds(5));
  ASSERT_TRUE(result);
  EXPECT_EQ(JavaQueryError::kTimeout, result->error);
  tasks.FastForwardBy(base::Seconds(10));
  EXPECT_EQ(1, callbacks);
  EXPECT_EQ(1u, network.connections.size());
}

TEST_F(JavaStatusQueryTest, CancelDuringSrvPreventsFallbackAndCallback) {
  Start();
  query.reset();
  tasks.FastForwardBy(base::Seconds(10));
  EXPECT_TRUE(network.resolutions[0]->cancelled);
  Reply(0, net::OK, {}, {net::HostPortPair("late.test", 25570)});
  EXPECT_EQ(1u, network.resolutions.size());
  EXPECT_TRUE(network.connections.empty());
  EXPECT_EQ(0, callbacks);
}

TEST_F(JavaStatusQueryTest, CancelDuringAddressLookupPreventsConnect) {
  Start(true);
  query->Cancel();
  Reply(0, net::OK, Addresses());
  EXPECT_TRUE(network.resolutions[0]->cancelled);
  EXPECT_TRUE(network.connections.empty());
  EXPECT_EQ(0, callbacks);
}

TEST_F(JavaStatusQueryTest, CancelDuringConnectClosesLateSuccessfulPipes) {
  Start(true);
  Reply(0, net::OK, Addresses());
  query.reset();
  network.CompleteConnect(0);
  tasks.RunUntilIdle();
  EXPECT_TRUE(
      network.connections[0]->socket->QuerySignalsState().peer_closed());
  EXPECT_TRUE(
      network.connections[0]->response->QuerySignalsState().peer_closed());
  EXPECT_TRUE(
      network.connections[0]->request->QuerySignalsState().peer_closed());
  EXPECT_EQ(0, callbacks);
}

TEST_F(JavaStatusQueryTest, CancelDuringReadClosesAllHandlesAndTimers) {
  ConnectDirect();
  query.reset();
  tasks.FastForwardBy(base::Seconds(10));
  auto& connection = *network.connections[0];
  EXPECT_TRUE(connection.socket->QuerySignalsState().peer_closed());
  EXPECT_TRUE(connection.response->QuerySignalsState().peer_closed());
  EXPECT_TRUE(connection.request->QuerySignalsState().peer_closed());
  EXPECT_FALSE(connection.observer.is_connected());
  EXPECT_EQ(0, callbacks);
}

TEST_F(JavaStatusQueryTest, ClosedContextIsAnImmediateFailure) {
  context_alive = false;
  Start();
  ASSERT_TRUE(result);
  EXPECT_EQ(JavaQueryError::kNetwork, result->error);
  EXPECT_TRUE(network.resolutions.empty());
  EXPECT_TRUE(network.connections.empty());
  EXPECT_EQ(1, callbacks);
}

TEST_F(JavaStatusQueryTest, ContextShutdownBetweenSrvAndAddressesStopsQuery) {
  Start();
  context_alive = false;
  Reply(0, net::OK, {}, {net::HostPortPair("backend.test", 25570)});
  ASSERT_TRUE(result);
  EXPECT_EQ(JavaQueryError::kNetwork, result->error);
  EXPECT_EQ(1u, network.resolutions.size());
  EXPECT_TRUE(network.connections.empty());
}

TEST_F(JavaStatusQueryTest, ResolverDisconnectFallsBackOnce) {
  Start();
  network.resolutions[0]->client.reset();
  tasks.RunUntilIdle();
  ASSERT_EQ(2u, network.resolutions.size());
  EXPECT_EQ("logical.test", network.resolutions[1]->host.host());
  Reply(1, net::ERR_NAME_NOT_RESOLVED);
  ASSERT_TRUE(result);
  EXPECT_EQ(JavaQueryError::kDns, result->error);
  EXPECT_EQ(1, callbacks);
}

TEST_F(JavaStatusQueryTest, SrvTargetLookupTimeoutAdvancesToNextCandidate) {
  Start();
  Reply(0, net::OK, {},
        {net::HostPortPair("one.test", 1), net::HostPortPair("two.test", 2)});
  tasks.FastForwardBy(base::Seconds(1));
  ASSERT_EQ(3u, network.resolutions.size());
  EXPECT_TRUE(network.resolutions[1]->cancelled);
  EXPECT_EQ("two.test", network.resolutions[2]->host.host());
  Reply(2, net::OK, Addresses());
  request = Connect();
  Send(StatusFrame(R"({"description":"second target"})"));
  Pong();
  ASSERT_TRUE(result && result->status);
  EXPECT_EQ(net::HostPortPair("two.test", 2), result->srv_target);
}

TEST_F(JavaStatusQueryTest, WriteErrorDrainsBufferedStatus) {
  ConnectDirect();
  Send(StatusFrame(R"({"description":"buffered before write error"})"), 0,
       false);
  network.connections[0]->observer->OnWriteError(net::ERR_CONNECTION_RESET);
  tasks.RunUntilIdle();
  ASSERT_TRUE(result && result->status);
  EXPECT_EQ("buffered before write error", result->status->motd);
  EXPECT_FALSE(result->rtt);
  EXPECT_EQ(1, callbacks);
}

TEST_F(JavaStatusQueryTest, PongWaitCannotExtendTheTotalDeadline) {
  ConnectDirect();
  const auto start = base::TimeTicks::Now();
  tasks.FastForwardBy(base::Milliseconds(4500));
  Send(StatusFrame(R"({"description":"late status"})"));
  tasks.FastForwardBy(base::Milliseconds(500));
  ASSERT_TRUE(result && result->status);
  EXPECT_EQ(base::Seconds(5), base::TimeTicks::Now() - start);
  tasks.FastForwardBy(base::Seconds(10));
  EXPECT_EQ(1, callbacks);
}

TEST_F(JavaStatusQueryTest, ValidStatusSurvivesMalformedPong) {
  ConnectDirect();
  Send(StatusFrame(R"({"description":"kept"})"));
  Send(std::vector<uint8_t>{2, 1, 0});
  ASSERT_TRUE(result && result->status);
  EXPECT_EQ(JavaQueryError::kNone, result->error);
  EXPECT_FALSE(result->rtt);
  EXPECT_FALSE(result->latency_diagnostic.empty());
}

}  // namespace
}  // namespace mcje
