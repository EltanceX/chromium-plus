// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/mcje_result_page.h"

#include "base/strings/escape.h"
#include "base/test/task_environment.h"
#include "chrome/browser/minecraft/server_status_response.h"
#include "mojo/public/cpp/system/data_pipe_utils.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "services/network/test/test_url_loader_client.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace mcje {
namespace {

TEST(McjeResultPageTest, EscapesAllRemoteTextAndIgnoresOriginalFavicon) {
  const std::string attack = "<img src=x onerror=\"attack()\">&'";
  JavaQueryResult result;
  result.status.emplace();
  result.status->motd = attack;
  result.status->version_name = attack;
  result.status->favicon = "UNTRUSTED_FAVICON";
  result.status->json_summary = attack;
  result.endpoint = attack;
  result.discovery = attack;
  result.latency_diagnostic = attack;
  result.completed_at = base::Time::UnixEpoch();
  const auto html = RenderResultPage(GURL("mcje://localhost/"), result, {});
  EXPECT_EQ(std::string::npos, html.find(attack));
  EXPECT_NE(std::string::npos, html.find(base::EscapeForHTML(attack)));
  EXPECT_EQ(std::string::npos, html.find("UNTRUSTED_FAVICON"));
  EXPECT_EQ(std::string::npos, html.find("<img "));
  EXPECT_NE(std::string::npos, html.find("id=icon-placeholder"));
  EXPECT_NE(std::string::npos, html.find("1970-01-01 00:00:00 UTC"));
}

TEST(McjeResultPageTest, QueryAndAddressErrorsAreEscaped) {
  JavaQueryResult result;
  result.message = "<script>bad</script>";
  auto html = RenderResultPage(GURL("mcje://localhost/"), result, {});
  EXPECT_NE(std::string::npos, html.find("查询失败"));
  EXPECT_NE(std::string::npos, html.find("&lt;script&gt;bad&lt;/script&gt;"));
  html = RenderResultPage(GURL("mcje://localhost/path"), result, {});
  EXPECT_NE(std::string::npos, html.find("地址无效"));
}

TEST(McjeResultPageTest, EscapingWorstCaseStatusStillFitsPageBudget) {
  JavaQueryResult result;
  result.status.emplace();
  result.status->motd.assign(kMaxTextBytes, '"');
  result.status->version_name = std::string(kMaxTextBytes, '"');
  result.status->json_summary.assign(kMaxJsonSummaryBytes, '"');
  const auto html = RenderResultPage(GURL("mcje://localhost/"), result, {});
  // Include a maximal encoded icon and fixed <img> markup in the accounting.
  EXPECT_LE(html.size() + kMaxFaviconDataUrlBytes + 256,
            minecraft::kMaxResultPageBytes);
}

class MinecraftResponseTest : public testing::Test {
 protected:
  base::test::TaskEnvironment tasks;
};

TEST_F(MinecraftResponseTest, ProtocolSpecificCspAndExactBody) {
  for (auto images : {minecraft::ResultPageImages::kNone,
                      minecraft::ResultPageImages::kData}) {
    network::TestURLLoaderClient client;
    const std::string html = "<!doctype html><p>Java 世界 &amp;</p>";
    minecraft::SendResultPage(&client, html, images);
    ASSERT_TRUE(client.has_received_completion());
    ASSERT_TRUE(client.response_head());
    EXPECT_EQ(net::OK, client.completion_status().error_code);
    const auto csp = client.response_head()->headers->GetNormalizedHeader(
        "Content-Security-Policy");
    ASSERT_TRUE(csp);
    EXPECT_NE(std::string::npos, csp->find("default-src 'none'"));
    EXPECT_EQ(images == minecraft::ResultPageImages::kData,
              csp->find("img-src data:") != std::string::npos);
    EXPECT_EQ("no-store", client.response_head()->headers->GetNormalizedHeader(
                              "Cache-Control"));
    EXPECT_EQ("nosniff", client.response_head()->headers->GetNormalizedHeader(
                             "X-Content-Type-Options"));
    EXPECT_EQ("no-referrer",
              client.response_head()->headers->GetNormalizedHeader(
                  "Referrer-Policy"));
    std::string body;
    EXPECT_TRUE(
        mojo::BlockingCopyToString(client.response_body_release(), &body));
    EXPECT_EQ(html, body);
    EXPECT_EQ(static_cast<int64_t>(html.size()),
              client.response_head()->content_length);
  }
}

TEST_F(MinecraftResponseTest, ActualHtmlLimitIsEnforcedBeforeSendingResponse) {
  network::TestURLLoaderClient accepted;
  minecraft::SendResultPage(&accepted,
                            std::string(minecraft::kMaxResultPageBytes, 'x'),
                            minecraft::ResultPageImages::kData);
  EXPECT_EQ(net::OK, accepted.completion_status().error_code);
  network::TestURLLoaderClient rejected;
  minecraft::SendResultPage(
      &rejected, std::string(minecraft::kMaxResultPageBytes + 1, 'x'),
      minecraft::ResultPageImages::kData);
  EXPECT_FALSE(rejected.has_received_response());
  EXPECT_EQ(net::ERR_INSUFFICIENT_RESOURCES,
            rejected.completion_status().error_code);
}

}  // namespace
}  // namespace mcje
