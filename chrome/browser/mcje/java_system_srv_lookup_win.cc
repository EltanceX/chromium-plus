// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/java_system_srv_lookup.h"

#include <windows.h>

#include <windns.h>

#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/rand_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/net_errors.h"

namespace mcje {
namespace {

// The OS holds a reference until its asynchronous completion callback. A
// cancelled navigation releases its own reference immediately; DNS records and
// request storage remain alive until Windows acknowledges completion.
class WindowsSrvRequest : public base::RefCountedThreadSafe<WindowsSrvRequest> {
 public:
  WindowsSrvRequest(std::string name, JavaSystemSrvLookup::Callback callback)
      : name_(base::UTF8ToWide(name)),
        callback_(std::move(callback)),
        runner_(base::SequencedTaskRunner::GetCurrentDefault()) {}

  void Start() {
    request_.Version = DNS_QUERY_REQUEST_VERSION1;
    request_.QueryName = name_.c_str();
    request_.QueryType = DNS_TYPE_SRV;
    request_.QueryOptions = DNS_QUERY_STANDARD | DNS_QUERY_TREAT_AS_FQDN;
    request_.pQueryCompletionCallback = &OnNativeComplete;
    request_.pQueryContext = this;
    result_.Version = DNS_QUERY_RESULTS_VERSION1;
    AddRef();  // Owned by the native completion, or released on sync return.
    const auto status = ::DnsQueryEx(&request_, &result_, &cancel_);
    pending_ = status == DNS_REQUEST_PENDING;
    if (!pending_) {
      Release();
      Complete();
    }
  }

  void Cancel() {
    callback_.Reset();
    if (pending_) {
      pending_ = false;
      // Completion can already be queued on this sequence. Cancellation may
      // then be a no-op, but the native request storage is still alive.
      ::DnsCancelQuery(&cancel_);
    }
  }

 private:
  friend class base::RefCountedThreadSafe<WindowsSrvRequest>;
  ~WindowsSrvRequest() {
    if (result_.pQueryRecords) {
      ::DnsRecordListFree(result_.pQueryRecords, DnsFreeRecordList);
    }
  }

  static void WINAPI OnNativeComplete(void* context, DNS_QUERY_RESULT*) {
    auto request =
        base::WrapRefCounted(static_cast<WindowsSrvRequest*>(context));
    request->Release();  // Transfer the OS reference to the posted callback.
    request->runner_->PostTask(
        FROM_HERE, base::BindOnce(&WindowsSrvRequest::Complete, request));
  }

  void Complete() {
    pending_ = false;
    if (!callback_) {
      return;
    }
    std::vector<JavaSrvRecord> records;
    size_t scanned = 0;
    if (result_.QueryStatus == ERROR_SUCCESS) {
      for (const auto* record = result_.pQueryRecords;
           record && scanned++ < 64 && records.size() < 16;
           record = record->pNext) {
        if (record->wType != DNS_TYPE_SRV ||
            record->Flags.S.Section != DnsSectionAnswer ||
            !record->Data.SRV.pNameTarget) {
          continue;
        }
        std::string host = base::WideToUTF8(record->Data.SRV.pNameTarget);
        if (host.ends_with('.')) {
          host.pop_back();
        }
        records.push_back({net::HostPortPair(host, record->Data.SRV.wPort),
                           record->Data.SRV.wPriority,
                           record->Data.SRV.wWeight});
      }
    }
    auto hosts = OrderJavaSrvRecords(std::move(records),
                                     base::BindRepeating(&base::RandGenerator));
    const int error = !hosts.empty() ? net::OK : net::ERR_NAME_NOT_RESOLVED;
    std::move(callback_).Run(error, std::move(hosts));
  }

  const std::wstring name_;
  JavaSystemSrvLookup::Callback callback_;
  const scoped_refptr<base::SequencedTaskRunner> runner_;
  DNS_QUERY_REQUEST request_ = {};
  DNS_QUERY_RESULT result_ = {};
  DNS_QUERY_CANCEL cancel_ = {};
  bool pending_ = false;  // Accessed only on the owning sequence.
};

class WindowsSrvLookup : public JavaSystemSrvLookup {
 public:
  ~WindowsSrvLookup() override { Cancel(); }
  void Start(std::string name, Callback callback) override {
    request_ = base::MakeRefCounted<WindowsSrvRequest>(std::move(name),
                                                       std::move(callback));
    // Synchronous cache completion can destroy this lookup via its callback.
    auto request = request_;
    request->Start();
  }
  void Cancel() override {
    if (request_) {
      request_->Cancel();
      request_.reset();
    }
  }

 private:
  scoped_refptr<WindowsSrvRequest> request_;
};

}  // namespace

std::unique_ptr<JavaSystemSrvLookup> CreateJavaSystemSrvLookup() {
  return std::make_unique<WindowsSrvLookup>();
}

}  // namespace mcje
