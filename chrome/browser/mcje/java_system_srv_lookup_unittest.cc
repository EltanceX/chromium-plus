// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/java_system_srv_lookup.h"

#include "base/functional/bind.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace mcje {
namespace {

TEST(JavaSystemSrvLookupTest, PriorityPrecedesWeight) {
  auto hosts = OrderJavaSrvRecords(
      {{net::HostPortPair("last.test", 3), 20, 65535},
       {net::HostPortPair("first.test", 1), 0, 0},
       {net::HostPortPair("middle.test", 2), 10, 0}},
      base::BindRepeating([](uint64_t upper) { return upper - 1; }));
  EXPECT_EQ((std::vector<net::HostPortPair>{
                {"first.test", 1}, {"middle.test", 2}, {"last.test", 3}}),
            hosts);
}

TEST(JavaSystemSrvLookupTest, WeightedSelectionIncludesZeroAndUpperBoundary) {
  std::vector<uint64_t> bounds;
  auto hosts = OrderJavaSrvRecords(
      {{net::HostPortPair("two.test", 2), 0, 2},
       {net::HostPortPair("zero.test", 1), 0, 0},
       {net::HostPortPair("three.test", 3), 0, 3}},
      base::BindRepeating(
          [](std::vector<uint64_t>* bounds, uint64_t upper) -> uint64_t {
            bounds->push_back(upper);
            return bounds->size() == 1 ? 0 : upper - 1;
          },
          &bounds));
  EXPECT_EQ((std::vector<uint64_t>{6, 6, 3}), bounds);
  EXPECT_EQ((std::vector<net::HostPortPair>{
                {"zero.test", 1}, {"three.test", 3}, {"two.test", 2}}),
            hosts);
}

TEST(JavaSystemSrvLookupTest, InvalidTargetsAreExcluded) {
  auto hosts = OrderJavaSrvRecords(
      {{net::HostPortPair(".", 25565), 0, 0},
       {net::HostPortPair("bad/path", 25565), 0, 0},
       {net::HostPortPair("zero-port.test", 0), 0, 0},
       {net::HostPortPair("valid.test", 11964), 0, 0}},
      base::BindRepeating([](uint64_t) -> uint64_t { return 0; }));
  EXPECT_EQ((std::vector<net::HostPortPair>{{"valid.test", 11964}}), hosts);
}

TEST(JavaSystemSrvLookupTest, RecordCountAndRandomWorkAreBounded) {
  std::vector<JavaSrvRecord> records;
  for (int port = 1; port <= 30; ++port) {
    records.push_back({net::HostPortPair("target.test", port), 0, 65535});
  }
  int draws = 0;
  auto hosts = OrderJavaSrvRecords(
      std::move(records), base::BindRepeating(
                              [](int* draws, uint64_t upper) -> uint64_t {
                                ++*draws;
                                EXPECT_LE(upper, 16u * 65535 + 1);
                                return upper - 1;
                              },
                              &draws));
  ASSERT_EQ(16u, hosts.size());
  EXPECT_EQ(16, draws);
  EXPECT_EQ(16, hosts.front().port());
  EXPECT_EQ(1, hosts.back().port());
}

}  // namespace
}  // namespace mcje
