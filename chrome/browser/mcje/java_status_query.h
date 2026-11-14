// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCJE_JAVA_STATUS_QUERY_H_
#define CHROME_BROWSER_MCJE_JAVA_STATUS_QUERY_H_

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/mcje/java_status_protocol.h"
#include "chrome/browser/mcje/java_status_resolver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "services/network/public/mojom/tcp_socket.mojom.h"

namespace mcje {

enum class JavaQueryError {
  kNone,
  kAddress,
  kDns,
  kConnect,
  kNetwork,
  kTimeout,
  kProtocol,
  kJson
};

struct JavaQueryResult {
  JavaQueryError error = JavaQueryError::kNone;
  std::string message;
  std::optional<JavaServerStatus> status;
  std::string endpoint;
  std::optional<net::HostPortPair> srv_target;
  std::string discovery;
  std::string latency_diagnostic;
  std::optional<base::TimeDelta> rtt;
  int connection_attempts = 0;
  base::Time completed_at;
};

// Navigation owns this single-use query. Cancel/destruction is silent; every
// other terminal path consumes the callback once, after releasing all I/O.
class JavaStatusQuery : public network::mojom::SocketObserver {
 public:
  using Callback = base::OnceCallback<void(JavaQueryResult)>;
  JavaStatusQuery(NetworkContextFactory context,
                  ServerAddress address,
                  JavaSystemSrvFactory system_srv_factory = {});
  ~JavaStatusQuery() override;
  void Start(Callback callback);
  void Cancel();

 private:
  void OnSrv(JavaHostResolution result);
  void FallbackFromSrv(std::string reason);
  void ResolveNextTarget();
  void OnAddresses(JavaHostResolution result);
  void TryEndpoint();
  void OnConnected(int attempt,
                   int result,
                   const std::optional<net::IPEndPoint>& local,
                   const std::optional<net::IPEndPoint>& peer,
                   mojo::ScopedDataPipeConsumerHandle receive,
                   mojo::ScopedDataPipeProducerHandle send);
  void OnConnectionFailure(int attempt, int error);
  void WriteMore(MojoResult result = MOJO_RESULT_OK);
  void ReadMore(MojoResult result = MOJO_RESULT_OK);
  void OnReadError(int error) override;
  void OnWriteError(int error) override;
  void OnSocketDisconnected(int attempt);
  void OnPhaseTimeout();
  void OnTimeout();
  void CompleteResponse(std::string reason);
  void FinishError(JavaQueryError error, std::string reason);
  void Finish(JavaQueryResult result);
  void CloseSocket();

  enum class Phase { kSrv, kAddresses, kConnect, kRead };
  const NetworkContextFactory context_;
  const JavaSystemSrvFactory system_srv_factory_;
  const ServerAddress address_;
  Callback callback_;
  bool started_ = false;
  Phase phase_ = Phase::kSrv;
  base::TimeTicks deadline_;
  base::OneShotTimer total_timer_;
  base::OneShotTimer phase_timer_;
  base::OneShotTimer pong_timer_;
  std::unique_ptr<JavaStatusResolver> resolver_;
  std::vector<net::HostPortPair> targets_;
  size_t next_target_ = 0;
  net::HostPortPair current_target_;
  bool using_srv_ = false;
  std::string discovery_;
  std::vector<net::IPEndPoint> endpoints_;
  size_t next_endpoint_ = 0;
  int attempts_ = 0;
  std::string endpoint_;
  std::unique_ptr<JavaStatusStreamParser> parser_;
  std::array<std::vector<uint8_t>, 3> outgoing_;
  size_t write_packet_ = 0;
  size_t write_offset_ = 0;
  base::TimeTicks ping_sent_at_;
  std::optional<base::TimeDelta> rtt_;
  std::optional<int> read_error_;
  mojo::Remote<network::mojom::TCPConnectedSocket> socket_;
  mojo::Receiver<network::mojom::SocketObserver> observer_{this};
  mojo::ScopedDataPipeConsumerHandle receive_;
  mojo::ScopedDataPipeProducerHandle send_;
  mojo::SimpleWatcher read_watcher_;
  mojo::SimpleWatcher write_watcher_;
  base::WeakPtrFactory<JavaStatusQuery> weak_factory_{this};
};

}  // namespace mcje

#endif  // CHROME_BROWSER_MCJE_JAVA_STATUS_QUERY_H_
