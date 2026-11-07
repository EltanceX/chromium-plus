// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCJE_MCJE_RESULT_PAGE_H_
#define CHROME_BROWSER_MCJE_MCJE_RESULT_PAGE_H_

#include <string>

#include "chrome/browser/mcje/java_server_icon.h"
#include "chrome/browser/mcje/java_status_query.h"
#include "url/gurl.h"

namespace mcje {

std::string RenderResultPage(const GURL& url,
                             const JavaQueryResult& result,
                             const JavaServerIcon& icon);

}  // namespace mcje

#endif  // CHROME_BROWSER_MCJE_MCJE_RESULT_PAGE_H_
