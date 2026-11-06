// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/settings/kernel_debug_handler.h"

#include <optional>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/numerics/safe_conversions.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/process/process_metrics.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "components/version_info/version_info.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_ui.h"

namespace settings {

KernelDebugHandler::KernelDebugHandler()
    : process_metrics_(base::ProcessMetrics::CreateCurrentProcessMetrics()) {}

KernelDebugHandler::~KernelDebugHandler() = default;

void KernelDebugHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "startKernelDebugMonitoring",
      base::BindRepeating(&KernelDebugHandler::HandleStartMonitoring,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "stopKernelDebugMonitoring",
      base::BindRepeating(&KernelDebugHandler::HandleStopMonitoring,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getKernelDebugTabDetails",
      base::BindRepeating(&KernelDebugHandler::HandleGetTabDetails,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "closeKernelDebugTab",
      base::BindRepeating(&KernelDebugHandler::HandleCloseTab,
                          base::Unretained(this)));
}

void KernelDebugHandler::OnJavascriptDisallowed() {
  snapshot_timer_.Stop();
}

void KernelDebugHandler::HandleStartMonitoring(const base::ListValue& args) {
  CHECK_EQ(0U, args.size());
  AllowJavascript();

  if (snapshot_timer_.IsRunning()) {
    return;
  }

  SendSnapshot();
  snapshot_timer_.Start(FROM_HERE, base::Seconds(2),
                        base::BindRepeating(&KernelDebugHandler::SendSnapshot,
                                            base::Unretained(this)));
}

void KernelDebugHandler::HandleStopMonitoring(const base::ListValue& args) {
  CHECK_EQ(0U, args.size());
  snapshot_timer_.Stop();
}

void KernelDebugHandler::HandleGetTabDetails(const base::ListValue& args) {
  CHECK_EQ(2U, args.size());
  AllowJavascript();

  const std::optional<int> tab_id = args[1].GetIfInt();
  base::Value details;
  if (tab_id.has_value()) {
    tabs::TabInterface* const tab = tabs::TabHandle(*tab_id).Get();
    const Profile* const profile = Profile::FromWebUI(web_ui());
    if (tab && tab->GetProfile() == profile) {
      base::DictValue tab_details;
      tab_details.Set("id", *tab_id);
      tab_details.Set("title", base::UTF16ToUTF8(tab->GetTitle()));
      tab_details.Set("isActive", tab->IsActivated());
      tab_details.Set("isVisible", tab->IsVisible());

      const base::Time last_active_time = tab->GetLastActiveTime();
      if (!last_active_time.is_null()) {
        tab_details.Set(
            "secondsSinceLastActive",
            base::saturated_cast<int>(
                (base::Time::Now() - last_active_time).InSeconds()));
      } else {
        tab_details.Set("secondsSinceLastActive", base::Value());
      }

      content::WebContents* const contents = tab->GetContents();
      if (contents) {
        tab_details.Set("url", contents->GetVisibleURL().spec());
        tab_details.Set("isLoading", contents->IsLoading());
        tab_details.Set("isCrashed", contents->IsCrashed());
        tab_details.Set("isAudible", contents->IsCurrentlyAudible());

        content::RenderFrameHost* const main_frame =
            contents->GetPrimaryMainFrame();
        if (main_frame && main_frame->GetProcess()) {
          tab_details.Set("rendererProcessId",
                          main_frame->GetProcess()->GetDeprecatedID());
        } else {
          tab_details.Set("rendererProcessId", base::Value());
        }
      }
      details = base::Value(std::move(tab_details));
    }
  }

  ResolveJavascriptCallback(args[0], details);
}

void KernelDebugHandler::HandleCloseTab(const base::ListValue& args) {
  CHECK_EQ(1U, args.size());

  const std::optional<int> tab_id = args[0].GetIfInt();
  if (!tab_id.has_value()) {
    return;
  }

  tabs::TabInterface* const tab = tabs::TabHandle(*tab_id).Get();
  const Profile* const profile = Profile::FromWebUI(web_ui());
  if (!tab || tab->GetProfile() != profile) {
    return;
  }

  BrowserWindowInterface* const browser = tab->GetBrowserWindowInterface();
  TabStripModel* const tab_strip_model =
      browser ? browser->GetTabStripModel() : nullptr;
  if (!tab_strip_model) {
    return;
  }

  const int index = tab_strip_model->GetIndexOfTab(tab);
  if (index == TabStripModel::kNoTab) {
    return;
  }

  tab_strip_model->CloseWebContentsAt(
      index, TabCloseTypes::CLOSE_USER_GESTURE |
                 TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB);
}

void KernelDebugHandler::SendSnapshot() {
  int browser_window_count = 0;
  int tab_count = 0;
  base::ListValue tab_list;
  const Profile* const profile = Profile::FromWebUI(web_ui());
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [profile, &browser_window_count, &tab_count,
       &tab_list](BrowserWindowInterface* browser) {
        if (browser->GetProfile() != profile) {
          return true;
        }

        ++browser_window_count;
        TabStripModel* const tab_strip_model = browser->GetTabStripModel();
        if (!tab_strip_model) {
          return true;
        }

        tab_count += tab_strip_model->count();
        for (int i = 0; i < tab_strip_model->count(); ++i) {
          tabs::TabInterface* const tab = tab_strip_model->GetTabAtIndex(i);
          if (!tab) {
            continue;
          }

          base::DictValue tab_info;
          tab_info.Set("id", tab->GetHandle().raw_value());
          tab_info.Set("title", base::UTF16ToUTF8(tab->GetTitle()));
          tab_info.Set("isActive", tab->IsActivated());
          tab_info.Set("windowNumber", browser_window_count);
          tab_list.Append(std::move(tab_info));
        }
        return true;
      });

  const base::Time creation_time = base::Process::Current().CreationTime();
  const int uptime_seconds =
      creation_time.is_null()
          ? 0
          : base::saturated_cast<int>(
                (base::Time::Now() - creation_time).InSeconds());

  base::DictValue snapshot;
  snapshot.Set("version", version_info::GetVersionNumber());
  snapshot.Set("processId",
               base::saturated_cast<int>(base::GetCurrentProcId()));
  snapshot.Set("uptimeSeconds", uptime_seconds);
  snapshot.Set("browserWindowCount", browser_window_count);
  snapshot.Set("tabCount", tab_count);
  snapshot.Set("tabs", std::move(tab_list));

  const auto memory_info = process_metrics_->GetMemoryInfo();
  if (memory_info.has_value()) {
    snapshot.Set("residentSetMb",
                 base::saturated_cast<int>(memory_info->resident_set_bytes /
                                           (1024 * 1024)));
  } else {
    snapshot.Set("residentSetMb", base::Value());
  }

  FireWebUIListener("kernel-debug-info-changed",
                    base::Value(std::move(snapshot)));
}

}  // namespace settings
