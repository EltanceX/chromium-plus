// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/functional/bind.h"
#include "base/test/launcher/unit_test_launcher.h"
#include "base/test/test_suite.h"
#include "third_party/blink/renderer/platform/testing/testing_platform_support.h"

int main(int argc, char** argv) {
  blink::ScopedUnittestsEnvironmentSetup environment(argc, argv);
  base::TestSuite suite(argc, argv);
  return base::LaunchUnitTests(
      argc, argv,
      base::BindOnce(&base::TestSuite::Run, base::Unretained(&suite)));
}
