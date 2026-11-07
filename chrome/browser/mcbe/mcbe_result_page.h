// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCBE_MCBE_RESULT_PAGE_H_
#define CHROME_BROWSER_MCBE_MCBE_RESULT_PAGE_H_

#include <string>

#include "chrome/browser/mcbe/bedrock_status_query.h"
#include "url/gurl.h"

namespace mcbe {

std::string RenderResultPage(const GURL& url, const QueryResult& result);

}  // namespace mcbe

#endif  // CHROME_BROWSER_MCBE_MCBE_RESULT_PAGE_H_
