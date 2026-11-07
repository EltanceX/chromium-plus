// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcbe/mcbe_result_page.h"

#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"

namespace mcbe {
namespace {

std::string Row(std::string_view label, std::string_view value) {
  return "<dt>" + base::EscapeForHTML(label) + "</dt><dd>" +
         base::EscapeForHTML(value.empty() ? "未提供" : value) + "</dd>";
}

std::string_view ErrorName(QueryError error) {
  switch (error) {
    case QueryError::kNone:
      return "查询成功";
    case QueryError::kAddress:
      return "地址无效";
    case QueryError::kDns:
      return "DNS 解析失败";
    case QueryError::kNetwork:
      return "网络查询失败";
    case QueryError::kTimeout:
      return "等待响应超时";
    case QueryError::kInvalidResponse:
      return "服务器响应无效";
  }
}

}  // namespace

std::string RenderResultPage(const GURL& url, const QueryResult& result) {
  std::string html = R"(<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Minecraft Bedrock 服务器状态</title><style>
:root{color-scheme:light dark;font:16px/1.6 system-ui,sans-serif}
body{margin:0;background:light-dark(#f3f6f4,#111815);color:light-dark(#152b20,#e3eee7)}
main{max-width:780px;margin:6vh auto;padding:32px;background:light-dark(#fff,#1b2720);border-radius:20px}
header{font-size:13px;letter-spacing:1px;color:light-dark(#337048,#9bd5ae)}
h1{font-size:30px;line-height:1.3;margin:12px 0;overflow-wrap:anywhere}
p{overflow-wrap:anywhere}.address,footer{color:light-dark(#52675a,#adbdaf)}
dl{display:grid;grid-template-columns:150px 1fr;gap:12px 20px;margin:28px 0}
dt{color:light-dark(#52675a,#adbdaf)}dd{margin:0;white-space:pre-wrap;overflow-wrap:anywhere}
pre{white-space:pre-wrap;overflow-wrap:anywhere;font:13px/1.6 monospace}
details{border-top:1px solid #78978055;padding-top:16px}footer{font-size:13px;margin-top:24px}
@media(max-width:600px){main{margin:12px;padding:22px}dl{grid-template-columns:105px 1fr;gap:10px}}
</style><main><header>MINECRAFT BEDROCK · UDP STATUS</header>)";
  html += "<h1>" +
          base::EscapeForHTML(result.status ? result.status->motd
                                            : ErrorName(result.error)) +
          "</h1><p class=address>" +
          base::EscapeForHTML(url.possibly_invalid_spec()) + "</p>";
  html +=
      "<p id=query-state>" + std::string(ErrorName(result.error)) + "</p><dl>";
  if (auto address = ParseServerAddress(url); address.has_value()) {
    html += Row("主机", address->host);
    html += Row("查询端口", base::NumberToString(address->port));
  }
  html += Row(result.status ? "响应端点" : "尝试端点", result.endpoint);
  if (result.status) {
    const auto& status = *result.status;
    html += Row("副标题", status.subtitle);
    html += Row("版本", status.version);
    html += Row("协议版本", base::NumberToString(status.protocol));
    html += Row("在线人数", base::NumberToString(status.players) + " / " +
                                base::NumberToString(status.max_players));
    html += Row("游戏模式", status.game_mode);
    html += Row("服务器 GUID", status.server_guid);
    if (!status.advertised_guid.empty() &&
        status.advertised_guid != status.server_guid) {
      html += Row("MOTD GUID", status.advertised_guid);
    }
    html += Row("往返耗时",
                base::StringPrintf("%.1f ms", result.rtt.InMillisecondsF()));
  } else {
    html += Row("原因", result.message);
  }
  base::Time::Exploded time;
  result.completed_at.UTCExplode(&time);
  html += Row("查询时间",
              base::StringPrintf("%04d-%02d-%02d %02d:%02d:%02d UTC", time.year,
                                 time.month, time.day_of_month, time.hour,
                                 time.minute, time.second));
  html += "</dl>";
  if (result.status) {
    html += "<details><summary>原始 MOTD</summary><pre>" +
            base::EscapeForHTML(result.status->raw_motd) + "</pre></details>";
  }
  html +=
      "<footer>"
      "使用浏览器刷新可重新查询。超时仅表示未收到响应，不能据此确认服务器离线。"
      "</footer></main></html>";
  return html;
}

}  // namespace mcbe
