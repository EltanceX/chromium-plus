// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcbe/mcbe_url_loader_factory.h"

#include <memory>
#include <utility>

#include "base/byte_size.h"
#include "base/functional/bind.h"
#include "base/memory/self_deleting.h"
#include "chrome/browser/mcbe/bedrock_status_query.h"
#include "chrome/browser/mcbe/mcbe_result_page.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace mcbe {
namespace {

network::mojom::NetworkContext* GetNetworkContext(
    base::WeakPtr<content::WebContents> contents,
    GURL url) {
  if (!contents || contents->GetBrowserContext()->ShutdownStarted()) {
    return nullptr;
  }
  return contents->GetBrowserContext()
      ->GetStoragePartitionForUrl(url)
      ->GetNetworkContext();
}

class McbeURLLoader final : public network::mojom::URLLoader,
                            public content::WebContentsObserver,
                            public base::SelfDeleting {
 public:
  McbeURLLoader(content::WebContents* contents,
                const GURL& url,
                mojo::PendingReceiver<network::mojom::URLLoader> receiver,
                mojo::PendingRemote<network::mojom::URLLoaderClient> client,
                base::SelfDeletingPassKey key)
      : WebContentsObserver(contents),
        SelfDeleting(key),
        url_(url),
        receiver_(this, std::move(receiver)),
        client_(std::move(client)) {
    receiver_.set_disconnect_handler(
        base::BindOnce(&McbeURLLoader::OnDisconnect, base::Unretained(this)));
    client_.set_disconnect_handler(
        base::BindOnce(&McbeURLLoader::OnDisconnect, base::Unretained(this)));
  }

  void Start() {
    auto address = ParseServerAddress(url_);
    if (!address.has_value()) {
      QueryResult result;
      result.error = QueryError::kAddress;
      result.message = address.error();
      result.completed_at = base::Time::Now();
      OnResult(std::move(result));
      return;
    }
    query_ = std::make_unique<BedrockStatusQuery>(
        base::BindRepeating(&GetNetworkContext, web_contents()->GetWeakPtr(),
                            url_),
        std::move(address.value()));
    query_->Start(
        base::BindOnce(&McbeURLLoader::OnResult, base::Unretained(this)));
  }

 private:
  ~McbeURLLoader() override = default;

  void FollowRedirect(network::HttpRequestHeadersUpdateParams,
                      const std::optional<GURL>&) override {}
  void SetPriority(net::RequestPriority, int32_t) override {}
  void WebContentsDestroyed() override {
    if (client_) {
      client_->OnComplete(network::URLLoaderCompletionStatus(net::ERR_ABORTED));
    }
    delete this;
  }
  void OnDisconnect() { delete this; }

  void OnResult(QueryResult result) {
    query_.reset();
    const std::string html = RenderResultPage(url_, result);
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    // All inputs are bounded. A pipe sized for the whole page avoids retaining
    // the loader in a separate asynchronous body producer after completion.
    if (html.size() > 512 * 1024 ||
        mojo::CreateDataPipe(html.size(), producer, consumer) !=
            MOJO_RESULT_OK ||
        producer->WriteAllData(base::as_byte_span(html)) != MOJO_RESULT_OK) {
      client_->OnComplete(
          network::URLLoaderCompletionStatus(net::ERR_INSUFFICIENT_RESOURCES));
      client_.reset();
      return;
    }
    producer.reset();
    auto head = network::mojom::URLResponseHead::New();
    head->mime_type = "text/html";
    head->charset = "utf-8";
    head->content_length = html.size();
    head->headers =
        net::HttpResponseHeaders::Builder(net::HttpVersion(1, 1), "200 OK")
            .AddHeader("Content-Type", "text/html; charset=utf-8")
            .AddHeader("Cache-Control", "no-store")
            .AddHeader(
                "Content-Security-Policy",
                "default-src 'none'; style-src 'unsafe-inline'; "
                "base-uri 'none'; form-action 'none'; frame-ancestors 'none'")
            .AddHeader("X-Content-Type-Options", "nosniff")
            .AddHeader("Referrer-Policy", "no-referrer")
            .Build();
    client_->OnReceiveResponse(std::move(head), std::move(consumer),
                               std::nullopt);
    network::URLLoaderCompletionStatus status(net::OK);
    status.encoded_data_length = base::ByteSize(html.size());
    status.encoded_body_length = base::ByteSize(html.size());
    status.decoded_body_length = base::ByteSize(html.size());
    client_->OnComplete(status);
    client_.reset();
  }

  const GURL url_;
  mojo::Receiver<network::mojom::URLLoader> receiver_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  std::unique_ptr<BedrockStatusQuery> query_;
};

class McbeURLLoaderFactory final
    : public network::SelfDeletingURLLoaderFactory {
 public:
  McbeURLLoaderFactory(
      content::FrameTreeNodeId frame_id,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      base::SelfDeletingPassKey key)
      : SelfDeletingURLLoaderFactory(std::move(receiver), key),
        frame_id_(frame_id) {}

 private:
  ~McbeURLLoaderFactory() override = default;

  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> pending_client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override {
    auto* contents = content::WebContents::FromFrameTreeNodeId(frame_id_);
    if (!contents || contents->GetBrowserContext()->ShutdownStarted() ||
        contents->GetPrimaryMainFrame()->GetFrameTreeNodeId() != frame_id_ ||
        request.method != "GET" ||
        request.mode != network::mojom::RequestMode::kNavigate ||
        request.destination != network::mojom::RequestDestination::kDocument) {
      mojo::Remote<network::mojom::URLLoaderClient> client(
          std::move(pending_client));
      client->OnComplete(
          network::URLLoaderCompletionStatus(net::ERR_ACCESS_DENIED));
      return;
    }
    base::MakeSelfDeleting<McbeURLLoader>(
        contents, request.url, std::move(loader), std::move(pending_client))
        ->Start();
  }

  const content::FrameTreeNodeId frame_id_;
};

}  // namespace

mojo::PendingRemote<network::mojom::URLLoaderFactory> CreateNavigationFactory(
    content::FrameTreeNodeId frame_tree_node_id) {
  mojo::PendingRemote<network::mojom::URLLoaderFactory> remote;
  base::MakeSelfDeleting<McbeURLLoaderFactory>(
      frame_tree_node_id, remote.InitWithNewPipeAndPassReceiver());
  return remote;
}

}  // namespace mcbe
