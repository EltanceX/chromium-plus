// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_NATIVE_PYTHON_NATIVE_PYTHON_SETTINGS_H_
#define CHROME_BROWSER_NATIVE_PYTHON_NATIVE_PYTHON_SETTINGS_H_

#include <string>

#include "base/values.h"

class PrefRegistrySimple;
class PrefService;
namespace base {
class CommandLine;
}

namespace native_python_settings {

inline constexpr char kEnabledPref[] = "native_python.enabled";
inline constexpr char kUnrestrictedPref[] = "native_python.unrestricted_pages";
inline constexpr char kAllowedOriginPref[] = "native_python.allowed_origin";

struct Configuration {
  bool enabled = false;
  bool unrestricted = false;
  std::string allowed_origin;
  bool operator==(const Configuration&) const = default;
};

bool IsSupported();
void RegisterLocalStatePrefs(PrefRegistrySimple* registry);
Configuration ReadConfiguration(const PrefService& prefs,
                                const base::CommandLine& command_line);
// Snapshot at browser startup: saves affect all renderers only after relaunch.
// Do not mutate the browser command line; relaunch must not retain old
// settings.
void InitializeForProcess(const PrefService& prefs,
                          const base::CommandLine& command_line);
const Configuration& ForProcess();
void AppendRendererSwitches(base::CommandLine& command_line);
base::DictValue GetSettings(const PrefService& prefs);
bool SaveSettings(PrefService& prefs,
                  const base::DictValue& settings,
                  std::string* error);

}  // namespace native_python_settings

#endif  // CHROME_BROWSER_NATIVE_PYTHON_NATIVE_PYTHON_SETTINGS_H_
