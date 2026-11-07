// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/mcje_url.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "url/origin.h"
#include "url/url_util.h"

namespace mcje {
namespace {

class McjeURLTest : public testing::Test {
 protected:
  void SetUp() override {
    url::AddStandardScheme("mcje",
                           url::SCHEME_WITH_HOST_PORT_AND_USER_INFORMATION);
    url::AddNoAccessScheme("mcje");
  }
  url::ScopedSchemeRegistryForTests registry_;
};

TEST_F(McjeURLTest, CanonicalDomainAndImplicitPort) {
  const GURL url("MCJE://MC.Hypixel.NET");
  auto address = ParseServerAddress(url);
  ASSERT_TRUE(address.has_value()) << address.error();
  EXPECT_EQ("mcje://mc.hypixel.net/", url.spec());
  EXPECT_EQ("mc.hypixel.net", address->logical_host);
  EXPECT_EQ(25565, address->logical_port);
  EXPECT_FALSE(address->port_was_explicit);
  EXPECT_FALSE(address->is_ip_literal);
  EXPECT_TRUE(address->ShouldQuerySrv());
}

TEST_F(McjeURLTest, ExplicitDefaultPortRetainsItsMeaning) {
  const GURL url("mcje://example.com:25565/");
  auto address = ParseServerAddress(url);
  ASSERT_TRUE(address.has_value());
  EXPECT_EQ("25565", url.port());
  EXPECT_EQ(25565, address->logical_port);
  EXPECT_TRUE(address->port_was_explicit);
  EXPECT_FALSE(address->is_ip_literal);
  EXPECT_FALSE(address->ShouldQuerySrv());

  address = ParseServerAddress(GURL("mcje://example.com:025565/"));
  ASSERT_TRUE(address.has_value());
  EXPECT_EQ(25565, address->logical_port);
  EXPECT_TRUE(address->port_was_explicit);
  EXPECT_FALSE(address->ShouldQuerySrv());
}

TEST_F(McjeURLTest, ExplicitPortsRespectTheirRange) {
  for (int port : {1, 25570, 65535}) {
    auto address = ParseServerAddress(
        GURL("mcje://example.com:" + std::to_string(port) + "/"));
    ASSERT_TRUE(address.has_value()) << port;
    EXPECT_EQ(port, address->logical_port);
    EXPECT_TRUE(address->port_was_explicit);
    EXPECT_FALSE(address->ShouldQuerySrv());
  }
}

TEST_F(McjeURLTest, IPAddressesSkipSrvAndKeepLogicalPort) {
  for (const auto* text : {"mcje://127.0.0.1/", "mcje://127.1/",
                           "mcje://[::1]/", "mcje://[2001:db8::1]/"}) {
    auto address = ParseServerAddress(GURL(text));
    ASSERT_TRUE(address.has_value()) << text;
    EXPECT_EQ(25565, address->logical_port);
    EXPECT_FALSE(address->port_was_explicit);
    EXPECT_TRUE(address->is_ip_literal);
    EXPECT_FALSE(address->ShouldQuerySrv());
  }
  auto address = ParseServerAddress(GURL("mcje://[2001:DB8::1]:25570/"));
  ASSERT_TRUE(address.has_value());
  EXPECT_EQ("2001:db8::1", address->logical_host);
  EXPECT_EQ(25570, address->logical_port);
  EXPECT_TRUE(address->port_was_explicit);
  EXPECT_TRUE(address->is_ip_literal);
  EXPECT_FALSE(address->ShouldQuerySrv());
}

TEST_F(McjeURLTest, RejectsUnsupportedComponentsAndInvalidPorts) {
  for (const auto* text :
       {"https://example.com/", "mcbe://example.com/", "mcje://",
        "mcje://example.com:0/", "mcje://example.com:65536/",
        "mcje://example.com:-1/", "mcje://example.com:abc/",
        "mcje://example.com/path", "mcje://example.com/?query",
        "mcje://example.com/?", "mcje://example.com/#fragment",
        "mcje://example.com/#", "mcje://user@example.com/",
        "mcje://:password@example.com/", "mcje://user:password@example.com/"}) {
    EXPECT_FALSE(ParseServerAddress(GURL(text)).has_value()) << text;
  }
  EXPECT_FALSE(
      ParseServerAddress(GURL("mcje://example.com/" + std::string(2048, 'x')))
          .has_value());
}

TEST_F(McjeURLTest, EnforcesHandshakeHostLength) {
  const std::string host = std::string(63, 'a') + "." + std::string(63, 'b') +
                           "." + std::string(63, 'c') + "." +
                           std::string(63, 'd');
  ASSERT_EQ(255u, host.size());
  auto address = ParseServerAddress(GURL("mcje://" + host + "/"));
  ASSERT_TRUE(address.has_value());
  EXPECT_EQ(host, address->logical_host);
  EXPECT_FALSE(ParseServerAddress(GURL("mcje://" + host + "./")).has_value());
}

TEST_F(McjeURLTest, StatusDocumentsHaveIndependentOpaqueOrigins) {
  const auto first = url::Origin::Create(GURL("mcje://example.com:25565/"));
  const auto second = url::Origin::Create(GURL("mcje://example.com:25565/"));
  const auto other_port =
      url::Origin::Create(GURL("mcje://example.com:25570/"));
  EXPECT_TRUE(first.opaque());
  EXPECT_TRUE(second.opaque());
  EXPECT_TRUE(other_port.opaque());
  EXPECT_EQ("null", first.Serialize());
  EXPECT_FALSE(first.IsSameOriginWith(second));
  EXPECT_FALSE(first.IsSameOriginWith(other_port));
}

}  // namespace
}  // namespace mcje
