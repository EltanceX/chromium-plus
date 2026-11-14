// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/java_system_srv_lookup.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "build/build_config.h"
#include "net/base/url_util.h"

namespace mcje {

std::vector<net::HostPortPair> OrderJavaSrvRecords(
    std::vector<JavaSrvRecord> records,
    base::RepeatingCallback<uint64_t(uint64_t)> random) {
  if (records.size() > 16) {
    records.resize(16);
  }
  std::erase_if(records, [](const auto& record) {
    return !record.target.port() ||
           !net::IsCanonicalizedHostCompliant(record.target.host());
  });
  std::stable_sort(records.begin(), records.end(),
                   [](const auto& a, const auto& b) {
                     if (a.priority != b.priority) {
                       return a.priority < b.priority;
                     }
                     return a.weight == 0 && b.weight != 0;
                   });
  std::vector<net::HostPortPair> ordered;
  while (!records.empty()) {
    const auto end = std::find_if(
        records.begin(), records.end(),
        [&](const auto& r) { return r.priority != records.front().priority; });
    uint64_t total = 0;
    for (auto it = records.begin(); it != end; ++it) {
      total += it->weight;
    }
    uint64_t draw = random.Run(total + 1);
    CHECK_LE(draw, total);
    auto chosen = records.begin();
    while (chosen->weight < draw) {
      draw -= chosen->weight;
      ++chosen;
      CHECK(chosen != end);
    }
    ordered.push_back(chosen->target);
    records.erase(chosen);
  }
  return ordered;
}

#if !BUILDFLAG(IS_WIN)
std::unique_ptr<JavaSystemSrvLookup> CreateJavaSystemSrvLookup() {
  return nullptr;
}
#endif

}  // namespace mcje
