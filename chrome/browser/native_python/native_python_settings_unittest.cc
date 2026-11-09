// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/native_python/native_python_settings.h"

#include "base/command_line.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace native_python_settings {
namespace {

class NativePythonSettingsTest : public testing::Test {
 protected:
  void SetUp() override { RegisterLocalStatePrefs(prefs_.registry()); }
  void TearDown() override {
    prefs_.SetBoolean(kEnabledPref, false);
    InitializeForProcess(prefs_, command_line_);
  }
  base::DictValue Settings(bool enabled,
                           bool unrestricted,
                           std::string origin) {
    return base::DictValue()
        .Set("enabled", enabled)
        .Set("unrestricted", unrestricted)
        .Set("allowedOrigin", origin);
  }
  TestingPrefServiceSimple prefs_;
  base::CommandLine command_line_{base::CommandLine::NO_PROGRAM};
};

TEST_F(NativePythonSettingsTest, DefaultsDoNotEnableOrRelaxPython) {
  const auto config = ReadConfiguration(prefs_, command_line_);
  EXPECT_FALSE(config.enabled);
  EXPECT_FALSE(config.unrestricted);
  EXPECT_TRUE(config.allowed_origin.empty());
}

TEST_F(NativePythonSettingsTest, SavedRestrictedOriginIsCanonicalAndValidated) {
  std::string error;
  ASSERT_TRUE(SaveSettings(
      prefs_, Settings(true, false, "http://LOCALHOST:80/"), &error));
  EXPECT_TRUE(ReadConfiguration(prefs_, command_line_).enabled);
  EXPECT_EQ("http://localhost", prefs_.GetString(kAllowedOriginPref));
  for (const char* invalid :
       {"", "https://example.com", "file:///test.html", "http://localhost/path",
        "http://localhost/*", "http://localhost,http://127.0.0.1"}) {
    EXPECT_FALSE(SaveSettings(prefs_, Settings(true, false, invalid), &error));
    EXPECT_EQ("http://localhost", prefs_.GetString(kAllowedOriginPref));
    EXPECT_FALSE(prefs_.GetBoolean(kUnrestrictedPref));
  }
}

TEST_F(NativePythonSettingsTest, UnrestrictedModeRequiresInterpreterEnabled) {
  std::string error;
  ASSERT_TRUE(SaveSettings(prefs_, Settings(false, true, ""), &error));
  EXPECT_FALSE(ReadConfiguration(prefs_, command_line_).unrestricted);
  ASSERT_TRUE(SaveSettings(prefs_, Settings(true, true, ""), &error));
  EXPECT_TRUE(ReadConfiguration(prefs_, command_line_).unrestricted);
}

TEST_F(NativePythonSettingsTest,
       CommandLineOriginAndDisableOverridePreferences) {
  prefs_.SetBoolean(kEnabledPref, true);
  prefs_.SetString(kAllowedOriginPref, "http://localhost:1");
  command_line_.AppendSwitchASCII("native-python-allowed-origin",
                                  "http://127.0.0.1:2");
  EXPECT_EQ("http://127.0.0.1:2",
            ReadConfiguration(prefs_, command_line_).allowed_origin);
  command_line_.AppendSwitchASCII("disable-blink-features", "PythonScript");
  command_line_.AppendSwitch("native-python-unrestricted-pages");
  const auto config = ReadConfiguration(prefs_, command_line_);
  EXPECT_FALSE(config.enabled);
  EXPECT_FALSE(config.unrestricted);
}

TEST_F(NativePythonSettingsTest, ExistingCommandLineOptInStillWorks) {
  command_line_.AppendSwitchASCII("enable-blink-features",
                                  "OtherFeature, PythonScript");
  command_line_.AppendSwitchASCII("native-python-allowed-origin",
                                  "http://127.0.0.1:3");
  const auto config = ReadConfiguration(prefs_, command_line_);
  EXPECT_TRUE(config.enabled);
  EXPECT_FALSE(config.unrestricted);
  EXPECT_EQ("http://127.0.0.1:3", config.allowed_origin);
}

TEST_F(NativePythonSettingsTest, SnapshotChangesOnlyAfterInitialization) {
  InitializeForProcess(prefs_, command_line_);
  std::string error;
  ASSERT_TRUE(SaveSettings(prefs_, Settings(true, true, ""), &error));
  EXPECT_FALSE(ForProcess().enabled);
  EXPECT_TRUE(*GetSettings(prefs_).FindBool("restartRequired"));
  InitializeForProcess(prefs_, command_line_);
  EXPECT_TRUE(ForProcess().enabled);
  EXPECT_TRUE(ForProcess().unrestricted);
  EXPECT_FALSE(command_line_.HasSwitch("enable-blink-features"));
  EXPECT_FALSE(*GetSettings(prefs_).FindBool("restartRequired"));
}

TEST_F(NativePythonSettingsTest, ChildSwitchesPreserveFeaturesAndSandbox) {
  prefs_.SetBoolean(kEnabledPref, true);
  prefs_.SetBoolean(kUnrestrictedPref, true);
  InitializeForProcess(prefs_, command_line_);
  base::CommandLine child(base::CommandLine::NO_PROGRAM);
  child.AppendSwitchASCII("enable-blink-features", "OtherFeature");
  AppendRendererSwitches(child);
  AppendRendererSwitches(child);
  EXPECT_EQ("OtherFeature,PythonScript",
            child.GetSwitchValueASCII("enable-blink-features"));
  EXPECT_TRUE(child.HasSwitch("native-python-unrestricted-pages"));
  EXPECT_FALSE(child.HasSwitch("no-sandbox"));
  EXPECT_FALSE(child.HasSwitch("disable-web-security"));
}

TEST_F(NativePythonSettingsTest, InvalidMessageDoesNotPartiallyWritePrefs) {
  auto value = Settings(true, true, "");
  value.Set("unrestricted", "true");
  std::string error;
  EXPECT_FALSE(SaveSettings(prefs_, value, &error));
  EXPECT_FALSE(prefs_.GetBoolean(kEnabledPref));
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace native_python_settings
