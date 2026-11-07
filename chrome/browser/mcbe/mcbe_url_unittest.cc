// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcbe/mcbe_url.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "url/origin.h"
#include "url/url_util.h"

namespace mcbe {
namespace {

class McbeURLTest : public testing::Test {
 protected:
  void SetUp() override {
    url::AddStandardScheme("mcbe",
                           url::SCHEME_WITH_HOST_PORT_AND_USER_INFORMATION);
    url::AddNoAccessScheme("mcbe");
  }
  url::ScopedSchemeRegistryForTests registry_;
};

TEST_F(McbeURLTest, DomainAndDefaultPort) {
  const GURL url("mcbe://GEO.hivebedrock.network");
  auto result = ParseServerAddress(url);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ("geo.hivebedrock.network", result->host);
  EXPECT_EQ(19132, result->port);
  EXPECT_TRUE(url::Origin::Create(url).opaque());
}

TEST_F(McbeURLTest, ExplicitPortsAndIPAddresses) {
  for (const auto* text : {"mcbe://example.com:1/", "mcbe://127.0.0.1:65535/",
                           "mcbe://[::1]:19133/", "mcbe://[2001:db8::1]/"}) {
    EXPECT_TRUE(ParseServerAddress(GURL(text)).has_value()) << text;
  }
  EXPECT_EQ(19133, ParseServerAddress(GURL("mcbe://[::1]:19133"))->port);
  EXPECT_EQ("::1", ParseServerAddress(GURL("mcbe://[::1]"))->host);
  EXPECT_EQ(19132, ParseServerAddress(GURL("mcbe://[::1]"))->port);
}

TEST_F(McbeURLTest, RejectsUnsupportedComponents) {
  for (const auto* text :
       {"http://example.com", "mcbe://", "mcbe://host:0", "mcbe://host:65536",
        "mcbe://host:abc", "mcbe://host/path", "mcbe://host?query",
        "mcbe://host?", "mcbe://host#fragment", "mcbe://host#",
        "mcbe://user:password@host/"}) {
    EXPECT_FALSE(ParseServerAddress(GURL(text)).has_value()) << text;
  }
  EXPECT_FALSE(ParseServerAddress(GURL("mcbe://host/" + std::string(2048, 'x')))
                   .has_value());
}

}  // namespace
}  // namespace mcbe
