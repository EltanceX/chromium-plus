// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/mcje_result_page.h"

#include <string_view>

#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"

namespace mcje {
namespace {

std::string Row(std::string_view id,
                std::string_view label,
                std::string_view value) {
  return "<dt>" + base::EscapeForHTML(label) + "</dt><dd id=\"" +
         std::string(id) + "\">" +
         base::EscapeForHTML(value.empty() ? "未提供" : value) + "</dd>";
}

std::string Number(std::optional<int> value) {
  return value ? base::NumberToString(*value) : "未提供";
}

}  // namespace

std::string RenderResultPage(const GURL& url,
                             const JavaQueryResult& result,
                             const JavaServerIcon& icon) {
  std::string html = R"(<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Minecraft Java 服务器状态</title><style>
:root{color-scheme:light dark;font:16px/1.6 system-ui,sans-serif}
body{margin:0;background:light-dark(#f3f6f4,#111815);color:light-dark(#152b20,#e3eee7)}
main{max-width:780px;margin:6vh auto;padding:32px;background:light-dark(#fff,#1b2720);border-radius:20px}
header{font-size:13px;letter-spacing:1px;color:light-dark(#337048,#9bd5ae)}
.heading{display:flex;align-items:center;gap:20px;margin:24px 0}
.icon{width:64px;height:64px;flex:none;border-radius:12px;background:light-dark(#e4ece6,#304438);display:grid;place-items:center;overflow:hidden}
.icon img{display:block;image-rendering:pixelated}.placeholder{font-size:12px;text-align:center}
h1{font-size:28px;line-height:1.3;margin:0;overflow-wrap:anywhere}
p,dd{overflow-wrap:anywhere}.muted,dt,footer{color:light-dark(#52675a,#adbdaf)}
#icon-state{margin:4px 0 0;font-size:13px}
dl{display:grid;grid-template-columns:130px 1fr;gap:12px 20px;margin:28px 0}
dd{margin:0;white-space:pre-wrap}pre{white-space:pre-wrap;overflow-wrap:anywhere;font:13px/1.6 monospace}
#motd{font:18px/1.6 system-ui,sans-serif;padding:20px;background:light-dark(#f3f6f4,#111815);border-radius:12px}
details{border-top:1px solid #78978055;padding-top:16px}footer{font-size:13px;margin-top:24px}
@media(max-width:600px){main{margin:12px;padding:22px}dl{grid-template-columns:100px 1fr;gap:10px}.heading{gap:14px}h1{font-size:24px}}
</style><main><header>MINECRAFT JAVA · TCP STATUS</header><div class=heading>)";
  if (result.status && !icon.data_url().empty()) {
    html +=
        "<div class=icon><img id=server-icon width=64 height=64 "
        "alt=\"Minecraft 服务器图标\" src=\"" +
        base::EscapeForHTML(icon.data_url()) + "\"></div>";
  } else {
    html +=
        "<div class=\"icon placeholder\" id=icon-placeholder "
        "role=img aria-label=\"无服务器图标\">无图标</div>";
  }
  html +=
      "<div><h1>Minecraft Java 服务器状态</h1><p class=muted id=icon-state>" +
      base::EscapeForHTML(result.status ? icon.diagnostic() : "未取得图标") +
      "</p></div></div><p class=muted>" +
      base::EscapeForHTML(url.possibly_invalid_spec()) + "</p>";
  const auto address = ParseServerAddress(url);
  if (!address.has_value()) {
    html += "<p id=address-state>地址无效</p><p id=query-error>" +
            base::EscapeForHTML(address.error()) + "</p>";
  } else {
    html += "<p id=address-state>地址已识别</p><p id=query-state>" +
            std::string(result.status ? "查询成功" : "查询失败") + "</p>";
    if (result.status) {
      html +=
          "<pre id=motd>" + base::EscapeForHTML(result.status->motd) + "</pre>";
    } else {
      html +=
          "<p id=query-error>" + base::EscapeForHTML(result.message) + "</p>";
    }
    html += "<dl>";
    html += Row("logical-host", "逻辑主机", address->logical_host);
    html += Row("logical-port", "逻辑端口",
                base::NumberToString(address->logical_port));
    html += Row("port-source", "端口来源",
                address->port_was_explicit ? "显式指定" : "默认端口");
    html += Row("endpoint", "实际端点", result.endpoint);
    html += Row("srv-target", "SRV 目标",
                result.srv_target ? result.srv_target->ToString() : "未使用");
    html += Row("discovery", "服务发现", result.discovery);
    html += Row("attempts", "连接次数",
                base::NumberToString(result.connection_attempts));
    if (result.status) {
      const auto& status = *result.status;
      html +=
          Row("version-name", "版本", status.version_name.value_or("未提供"));
      html +=
          Row("protocol-version", "协议版本", Number(status.protocol_version));
      html += Row("players-online", "在线人数", Number(status.players_online));
      html += Row("players-max", "人数上限", Number(status.players_max));
      html += Row("rtt", "往返延迟",
                  result.rtt ? base::StringPrintf("%.1f ms",
                                                  result.rtt->InMillisecondsF())
                             : "未取得");
      html += Row("latency-diagnostic", "延迟说明", result.latency_diagnostic);
    }
    if (!result.completed_at.is_null()) {
      base::Time::Exploded time;
      result.completed_at.UTCExplode(&time);
      html += Row("completed-at", "查询时间",
                  base::StringPrintf("%04d-%02d-%02d %02d:%02d:%02d UTC",
                                     time.year, time.month, time.day_of_month,
                                     time.hour, time.minute, time.second));
    }
    html += "</dl>";
    if (result.status) {
      html +=
          "<details><summary>响应 JSON 摘要</summary><pre id=json-summary>" +
          base::EscapeForHTML(result.status->json_summary) + "</pre></details>";
    }
  }
  html +=
      "<footer>"
      "使用浏览器刷新可重新查询。超时仅表示未收到响应，不能据此确认服务器离线。"
      "图标是状态响应中的可选 PNG 附件。</footer></main></html>";
  return html;
}

}  // namespace mcje
