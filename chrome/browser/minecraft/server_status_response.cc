// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/minecraft/server_status_response.h"

#include <string>
#include <utility>

#include "base/byte_size.h"
#include "base/containers/span.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace minecraft {

void SendResultPage(network::mojom::URLLoaderClient* client,
                    std::string_view html,
                    ResultPageImages images) {
  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  // Check the actual escaped HTML including the re-encoded icon. Sizing the
  // pipe for the whole page avoids an asynchronous producer after completion.
  if (html.empty() || html.size() > kMaxResultPageBytes ||
      mojo::CreateDataPipe(html.size(), producer, consumer) != MOJO_RESULT_OK ||
      producer->WriteAllData(base::as_byte_span(html)) != MOJO_RESULT_OK) {
    client->OnComplete(
        network::URLLoaderCompletionStatus(net::ERR_INSUFFICIENT_RESOURCES));
    return;
  }
  producer.reset();
  std::string csp = "default-src 'none'; style-src 'unsafe-inline'; ";
  if (images == ResultPageImages::kData) {
    csp += "img-src data:; ";
  }
  csp += "base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
  auto head = network::mojom::URLResponseHead::New();
  head->mime_type = "text/html";
  head->charset = "utf-8";
  head->content_length = html.size();
  head->headers =
      net::HttpResponseHeaders::Builder(net::HttpVersion(1, 1), "200 OK")
          .AddHeader("Content-Type", "text/html; charset=utf-8")
          .AddHeader("Cache-Control", "no-store")
          .AddHeader("Content-Security-Policy", csp)
          .AddHeader("X-Content-Type-Options", "nosniff")
          .AddHeader("Referrer-Policy", "no-referrer")
          .Build();
  client->OnReceiveResponse(std::move(head), std::move(consumer), std::nullopt);
  network::URLLoaderCompletionStatus status(net::OK);
  status.encoded_data_length = base::ByteSize(html.size());
  status.encoded_body_length = base::ByteSize(html.size());
  status.decoded_body_length = base::ByteSize(html.size());
  client->OnComplete(status);
}

}  // namespace minecraft
