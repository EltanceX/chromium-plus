// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/mcje_url_loader_factory.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/memory/self_deleting.h"
#include "base/scoped_observation.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/mcje/java_server_icon.h"
#include "chrome/browser/mcje/java_status_query.h"
#include "chrome/browser/mcje/mcje_result_page.h"
#include "chrome/browser/mcje/mcje_url.h"
#include "chrome/browser/minecraft/server_status_response.h"
#include "chrome/browser/net/secure_dns_config.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader.mojom.h"

namespace mcje {
namespace {

std::unique_ptr<JavaSystemSrvLookup> MaybeCreateSystemSrvLookup() {
#if BUILDFLAG(IS_WIN)
  // Do not bypass an explicit restriction on extra query types or strict DoH.
  auto* local_state = g_browser_process->local_state();
  auto* dns_config = SystemNetworkContextManager::GetStubResolverConfigReader();
  if (!local_state || !dns_config ||
      !local_state->GetBoolean(prefs::kAdditionalDnsQueryTypesEnabled) ||
      dns_config->GetSecureDnsConfiguration(false).mode() ==
          net::SecureDnsMode::kSecure) {
    return nullptr;
  }
  return CreateJavaSystemSrvLookup();
#else
  return nullptr;
#endif
}

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

class McjeURLLoader final : public network::mojom::URLLoader,
                            public content::WebContentsObserver,
                            public ProfileObserver,
                            public base::SelfDeleting {
 public:
  McjeURLLoader(content::WebContents* contents,
                GURL url,
                mojo::PendingReceiver<network::mojom::URLLoader> receiver,
                mojo::PendingRemote<network::mojom::URLLoaderClient> client,
                base::SelfDeletingPassKey key)
      : WebContentsObserver(contents),
        SelfDeleting(key),
        url_(std::move(url)),
        receiver_(this, std::move(receiver)),
        client_(std::move(client)) {
    profile_observation_.Observe(
        Profile::FromBrowserContext(contents->GetBrowserContext()));
    receiver_.set_disconnect_handler(
        base::BindOnce(&McjeURLLoader::OnDisconnect, base::Unretained(this)));
    client_.set_disconnect_handler(
        base::BindOnce(&McjeURLLoader::OnDisconnect, base::Unretained(this)));
  }

  void Start() {
    auto address = ParseServerAddress(url_);
    if (!address.has_value()) {
      JavaQueryResult result;
      result.error = JavaQueryError::kAddress;
      result.message = address.error();
      result.completed_at = base::Time::Now();
      OnResult(std::move(result));
      return;
    }
    query_ = std::make_unique<JavaStatusQuery>(
        base::BindRepeating(&GetNetworkContext, web_contents()->GetWeakPtr(),
                            url_),
        std::move(*address), base::BindRepeating(&MaybeCreateSystemSrvLookup));
    query_->Start(
        base::BindOnce(&McjeURLLoader::OnResult, base::Unretained(this)));
  }

 private:
  void OnResult(JavaQueryResult result) {
    query_.reset();
    result_ = std::move(result);
    if (!result_->status) {
      OnIcon(JavaServerIcon());
      return;
    }
    icon_processor_ = std::make_unique<JavaServerIconProcessor>();
    icon_processor_->Start(
        *result_->status,
        base::BindOnce(&McjeURLLoader::OnIcon, base::Unretained(this)));
  }

  void OnIcon(JavaServerIcon icon) {
    icon_processor_.reset();
    // Never carry the original attachment into the result document.
    if (result_->status) {
      result_->status->favicon.reset();
    }
    minecraft::SendResultPage(client_.get(),
                              RenderResultPage(url_, *result_, icon),
                              minecraft::ResultPageImages::kData);
    result_.reset();
    client_.reset();
  }

  ~McjeURLLoader() override = default;

  void FollowRedirect(network::HttpRequestHeadersUpdateParams,
                      const std::optional<GURL>&) override {}
  void SetPriority(net::RequestPriority, int32_t) override {}
  void WebContentsDestroyed() override { Abort(); }
  void DidStartNavigation(content::NavigationHandle* navigation) override {
    if ((query_ || icon_processor_) && navigation->IsInPrimaryMainFrame() &&
        !navigation->IsSameDocument()) {
      Abort();
    }
  }
  void OnProfileWillBeDestroyed(Profile*) override {
    profile_observation_.Reset();
    Abort();
  }
  void Abort() {
    query_.reset();
    icon_processor_.reset();
    if (client_) {
      client_->OnComplete(network::URLLoaderCompletionStatus(net::ERR_ABORTED));
    }
    delete this;
  }
  void OnDisconnect() { delete this; }

  const GURL url_;
  base::ScopedObservation<Profile, ProfileObserver> profile_observation_{this};
  mojo::Receiver<network::mojom::URLLoader> receiver_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  std::unique_ptr<JavaStatusQuery> query_;
  std::unique_ptr<JavaServerIconProcessor> icon_processor_;
  std::optional<JavaQueryResult> result_;
};

class McjeURLLoaderFactory final
    : public network::SelfDeletingURLLoaderFactory {
 public:
  McjeURLLoaderFactory(
      content::FrameTreeNodeId frame_id,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      base::SelfDeletingPassKey key)
      : SelfDeletingURLLoaderFactory(std::move(receiver), key),
        frame_id_(frame_id) {}

 private:
  ~McjeURLLoaderFactory() override = default;

  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> pending_client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override {
    auto* contents = content::WebContents::FromFrameTreeNodeId(frame_id_);
    auto* main_frame = contents ? contents->GetPrimaryMainFrame() : nullptr;
    if (!main_frame || contents->GetBrowserContext()->ShutdownStarted() ||
        main_frame->GetFrameTreeNodeId() != frame_id_ ||
        request.method != "GET" ||
        request.mode != network::mojom::RequestMode::kNavigate ||
        request.destination != network::mojom::RequestDestination::kDocument) {
      mojo::Remote<network::mojom::URLLoaderClient> client(
          std::move(pending_client));
      client->OnComplete(
          network::URLLoaderCompletionStatus(net::ERR_ACCESS_DENIED));
      return;
    }
    base::MakeSelfDeleting<McjeURLLoader>(
        contents, request.url, std::move(loader), std::move(pending_client))
        ->Start();
  }

  const content::FrameTreeNodeId frame_id_;
};

}  // namespace

mojo::PendingRemote<network::mojom::URLLoaderFactory> CreateNavigationFactory(
    content::FrameTreeNodeId frame_tree_node_id) {
  mojo::PendingRemote<network::mojom::URLLoaderFactory> remote;
  base::MakeSelfDeleting<McjeURLLoaderFactory>(
      frame_tree_node_id, remote.InitWithNewPipeAndPassReceiver());
  return remote;
}

}  // namespace mcje
