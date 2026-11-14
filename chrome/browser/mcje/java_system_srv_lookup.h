// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCJE_JAVA_SYSTEM_SRV_LOOKUP_H_
#define CHROME_BROWSER_MCJE_JAVA_SYSTEM_SRV_LOOKUP_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "net/base/host_port_pair.h"

namespace mcje {

struct JavaSrvRecord {
  net::HostPortPair target;
  uint16_t priority;
  uint16_t weight;
};

// Bounds, validates and orders native SRV records by priority and RFC 2782
// weighted selection. Random returns a value in [0, upper_bound).
std::vector<net::HostPortPair> OrderJavaSrvRecords(
    std::vector<JavaSrvRecord> records,
    base::RepeatingCallback<uint64_t(uint64_t)> random);

class JavaSystemSrvLookup {
 public:
  using Callback =
      base::OnceCallback<void(int, std::vector<net::HostPortPair>)>;
  virtual ~JavaSystemSrvLookup() = default;
  virtual void Start(std::string name, Callback callback) = 0;
  // Cancels the OS operation and silently discards the result callback.
  virtual void Cancel() = 0;
};

using JavaSystemSrvFactory =
    base::RepeatingCallback<std::unique_ptr<JavaSystemSrvLookup>()>;

// Windows uses the configured OS DNS resolver, including VPN/split DNS. Other
// platforms currently have no fallback. The caller must first check policy.
std::unique_ptr<JavaSystemSrvLookup> CreateJavaSystemSrvLookup();

}  // namespace mcje

#endif  // CHROME_BROWSER_MCJE_JAVA_SYSTEM_SRV_LOOKUP_H_
