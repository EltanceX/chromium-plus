// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/native_python/native_python_settings.h"

#include "base/command_line.h"
#include "base/no_destructor.h"
#include "base/strings/string_split.h"
#include "build/build_config.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/native_python_policy.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"

namespace native_python_settings {
namespace {

constexpr char kEnableBlinkFeatures[] = "enable-blink-features";
constexpr char kDisableBlinkFeatures[] = "disable-blink-features";

bool ContainsPython(const base::CommandLine& command_line, const char* name) {
  const std::string features = command_line.GetSwitchValueASCII(name);
  for (auto feature : base::SplitStringPiece(
           features, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (feature == "PythonScript") {
      return true;
    }
  }
  return false;
}

Configuration& ProcessConfiguration() {
  static base::NoDestructor<Configuration> config;
  return *config;
}

bool CommandLineOverrides(const base::CommandLine& command_line) {
  return ContainsPython(command_line, kEnableBlinkFeatures) ||
         ContainsPython(command_line, kDisableBlinkFeatures) ||
         command_line.HasSwitch(switches::kNativePythonAllowedOrigin) ||
         command_line.HasSwitch(kNativePythonUnrestrictedPages);
}

}  // namespace

bool IsSupported() {
#if BUILDFLAG(IS_WIN) && defined(ARCH_CPU_X86_64)
  return true;
#else
  return false;
#endif
}

void RegisterLocalStatePrefs(PrefRegistrySimple* registry) {
  registry->RegisterBooleanPref(kEnabledPref, false);
  registry->RegisterBooleanPref(kUnrestrictedPref, false);
  registry->RegisterStringPref(kAllowedOriginPref, "");
}

Configuration ReadConfiguration(const PrefService& prefs,
                                const base::CommandLine& command_line) {
  Configuration config;
  config.enabled = IsSupported() &&
                   (prefs.GetBoolean(kEnabledPref) ||
                    ContainsPython(command_line, kEnableBlinkFeatures)) &&
                   !ContainsPython(command_line, kDisableBlinkFeatures);
  config.unrestricted =
      config.enabled &&
      (prefs.GetBoolean(kUnrestrictedPref) ||
       command_line.HasSwitch(kNativePythonUnrestrictedPages));
  const std::string value =
      command_line.HasSwitch(switches::kNativePythonAllowedOrigin)
          ? command_line.GetSwitchValueASCII(
                switches::kNativePythonAllowedOrigin)
          : prefs.GetString(kAllowedOriginPref);
  if (auto origin = ParseNativePythonAllowedOrigin(value)) {
    config.allowed_origin = origin->Serialize();
  }
  return config;
}

void InitializeForProcess(const PrefService& prefs,
                          const base::CommandLine& command_line) {
  ProcessConfiguration() = ReadConfiguration(prefs, command_line);
}

const Configuration& ForProcess() {
  return ProcessConfiguration();
}

void AppendRendererSwitches(base::CommandLine& command_line) {
  const auto& config = ForProcess();
  if (!config.allowed_origin.empty()) {
    command_line.AppendSwitchASCII(switches::kNativePythonAllowedOrigin,
                                   config.allowed_origin);
  }
  if (!config.enabled) {
    return;
  }
  if (!ContainsPython(command_line, kEnableBlinkFeatures)) {
    std::string features =
        command_line.GetSwitchValueASCII(kEnableBlinkFeatures);
    if (!features.empty()) {
      features += ",";
    }
    command_line.AppendSwitchASCII(kEnableBlinkFeatures,
                                   features + "PythonScript");
  }
  if (config.unrestricted) {
    command_line.AppendSwitch(kNativePythonUnrestrictedPages);
  }
}

base::DictValue GetSettings(const PrefService& prefs) {
  const auto& active = ForProcess();
  const auto& command_line = *base::CommandLine::ForCurrentProcess();
  return base::DictValue()
      .Set("supported", IsSupported())
      .Set("enabled", prefs.GetBoolean(kEnabledPref))
      .Set("unrestricted", prefs.GetBoolean(kUnrestrictedPref))
      .Set("allowedOrigin", prefs.GetString(kAllowedOriginPref))
      .Set("activeEnabled", active.enabled)
      .Set("activeUnrestricted", active.unrestricted)
      .Set("activeAllowedOrigin", active.allowed_origin)
      .Set("commandLineOverride", CommandLineOverrides(command_line))
      .Set("restartRequired", ReadConfiguration(prefs, command_line) != active);
}

bool SaveSettings(PrefService& prefs,
                  const base::DictValue& settings,
                  std::string* error) {
  const auto enabled = settings.FindBool("enabled");
  const auto unrestricted = settings.FindBool("unrestricted");
  const auto* origin_text = settings.FindString("allowedOrigin");
  if (!IsSupported() || !enabled || !unrestricted || !origin_text ||
      origin_text->size() > 2048) {
    *error = "Python 设置无效或当前平台不支持内嵌解释器。";
    return false;
  }
  const auto origin = ParseNativePythonAllowedOrigin(*origin_text);
  if ((!origin_text->empty() && !origin) ||
      (*enabled && !*unrestricted && !origin)) {
    *error =
        "受限模式需要完整的本地 HTTP(S) 来源，例如 "
        "http://127.0.0.1:8000；不支持路径、通配符或来源列表。";
    return false;
  }
  prefs.SetBoolean(kEnabledPref, *enabled);
  prefs.SetBoolean(kUnrestrictedPref, *unrestricted);
  prefs.SetString(kAllowedOriginPref, origin ? origin->Serialize() : "");
  prefs.CommitPendingWrite();
  return true;
}

}  // namespace native_python_settings
