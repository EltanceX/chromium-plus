// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_SETTINGS_KERNEL_DEBUG_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_SETTINGS_KERNEL_DEBUG_HANDLER_H_

#include <memory>

#include "base/timer/timer.h"
#include "chrome/browser/ui/webui/settings/settings_page_ui_handler.h"

namespace base {
class ProcessMetrics;
}

namespace settings {

class KernelDebugHandler : public SettingsPageUIHandler {
 public:
  KernelDebugHandler();
  KernelDebugHandler(const KernelDebugHandler&) = delete;
  KernelDebugHandler& operator=(const KernelDebugHandler&) = delete;
  ~KernelDebugHandler() override;

  void RegisterMessages() override;
  void OnJavascriptAllowed() override {}
  void OnJavascriptDisallowed() override;

 private:
  void HandleStartMonitoring(const base::ListValue& args);
  void HandleStopMonitoring(const base::ListValue& args);
  void HandleGetTabDetails(const base::ListValue& args);
  void HandleCloseTab(const base::ListValue& args);
  void SendSnapshot();

  std::unique_ptr<base::ProcessMetrics> process_metrics_;
  base::RepeatingTimer snapshot_timer_;
};

}  // namespace settings

#endif  // CHROME_BROWSER_UI_WEBUI_SETTINGS_KERNEL_DEBUG_HANDLER_H_
