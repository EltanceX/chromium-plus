// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/java_status_json.h"

#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "base/check.h"
#include "base/json/json_writer.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace mcje {
namespace {

std::string WithDescription(base::Value description) {
  auto json = base::WriteJson(
      base::DictValue().Set("description", std::move(description)));
  CHECK(json);
  return *json;
}

base::Value NestedText(size_t depth) {
  base::Value value("x");
  for (size_t i = 1; i < depth; ++i) {
    value = base::Value(base::ListValue().Append(std::move(value)));
  }
  return value;
}

TEST(JavaStatusJsonTest, Utf8FormattingAndLineBreaksRemainPlainText) {
  auto status = ParseJavaStatusJson(
      R"({"description":"§aHello §R世界\n<img src=x onerror=run()> & §z§g§"})");
  ASSERT_TRUE(status.has_value()) << status.error();
  EXPECT_EQ("Hello 世界\n<img src=x onerror=run()> & §z§g§", status->motd);
  EXPECT_FALSE(status->version_name);
  EXPECT_FALSE(status->protocol_version);
  EXPECT_FALSE(status->players_online);
  EXPECT_FALSE(status->players_max);
  EXPECT_FALSE(status->favicon);
}

TEST(JavaStatusJsonTest, ArraysExtraAndEventMetadata) {
  auto status = ParseJavaStatusJson(R"({"description":["A",{
    "text":"B","extra":["C",{"text":"D","extra":["\n世界"]}],
    "clickEvent":{"action":"run_command","value":"/bad"},
    "hoverEvent":{"contents":{"text":"invisible"}},"color":"red"}]})");
  ASSERT_TRUE(status.has_value()) << status.error();
  EXPECT_EQ("ABCD\n世界", status->motd);
  auto extra_only = ParseJavaStatusJson(R"({"description":{"extra":["x"]}})");
  ASSERT_TRUE(extra_only.has_value());
  EXPECT_EQ("x", extra_only->motd);
  EXPECT_TRUE(ParseJavaStatusJson(R"({"description":[]})").has_value());
}

TEST(JavaStatusJsonTest, TranslationFallbackOrKeyKeepsBoundedArguments) {
  auto status = ParseJavaStatusJson(R"({"description":[
    {"translate":"server.welcome","fallback":"Welcome %s",
     "with":[{"text":"Alex"},"世界"]},
    {"translate":"server.unknown","with":["arg"]},
    {"translate":"server.empty","with":[]}]})");
  ASSERT_TRUE(status.has_value()) << status.error();
  EXPECT_EQ("Welcome %s [Alex, 世界]server.unknown [arg]server.empty",
            status->motd);
}

TEST(JavaStatusJsonTest, RejectsUnsupportedAndMistypedDescriptionComponents) {
  for (const char* component :
       {"null", "true", "1", "{}", R"({"color":"red"})",
        R"({"score":{"name":"x"}})", R"({"text":1})", R"({"translate":true})",
        R"({"translate":"x","fallback":1})",
        R"({"translate":"x","with":"bad"})", R"({"text":"x","extra":{}})",
        R"(["x",false])", R"({"translate":"x","with":[1]})"}) {
    EXPECT_FALSE(
        ParseJavaStatusJson("{\"description\":" + std::string(component) + "}")
            .has_value())
        << component;
  }
}

TEST(JavaStatusJsonTest, RequiresObjectAndDescription) {
  for (const char* json :
       {"null", "[]", "true", "1", "{}", R"({"version":{}})"}) {
    EXPECT_FALSE(ParseJavaStatusJson(json).has_value()) << json;
  }
}

TEST(JavaStatusJsonTest, OptionalFieldsAndExtraData) {
  auto status = ParseJavaStatusJson(R"({"description":"ok",
    "version":{"name":"1.21 test","protocol":767},
    "players":{"online":200,"max":100,"sample":"ignored"},
    "unknown":{"future":true}})");
  ASSERT_TRUE(status.has_value()) << status.error();
  EXPECT_EQ("1.21 test", status->version_name);
  EXPECT_EQ(767, status->protocol_version);
  EXPECT_EQ(200, status->players_online);
  EXPECT_EQ(100, status->players_max);
  auto missing = ParseJavaStatusJson(
      R"({"description":"ok","version":{},"players":{"online":0}})");
  ASSERT_TRUE(missing.has_value());
  EXPECT_FALSE(missing->version_name);
  EXPECT_FALSE(missing->protocol_version);
  EXPECT_EQ(0, missing->players_online);
  EXPECT_FALSE(missing->players_max);
}

TEST(JavaStatusJsonTest, IntegerBoundariesAndIntegralJsonNumbers) {
  auto status = ParseJavaStatusJson(R"({"description":"ok",
    "version":{"protocol":-2147483648},
    "players":{"online":2147483647,"max":1.0}})");
  ASSERT_TRUE(status.has_value()) << status.error();
  EXPECT_EQ(std::numeric_limits<int>::min(), status->protocol_version);
  EXPECT_EQ(std::numeric_limits<int>::max(), status->players_online);
  EXPECT_EQ(1, status->players_max);
}

TEST(JavaStatusJsonTest, RejectsMistypedOptionalFieldsAndInvalidNumbers) {
  for (const char* fields :
       {R"("version":null)", R"("version":[])", R"("version":{"name":1})",
        R"("version":{"protocol":"47"})",
        R"("version":{"protocol":2147483648})",
        R"("version":{"protocol":-2147483649})", R"("players":false)",
        R"("players":{"online":-1})", R"("players":{"max":-1})",
        R"("players":{"online":1.5})", R"("players":{"max":2147483648})",
        R"("players":{"online":null})", R"("players":{"max":"10"})",
        R"("players":{"online":true})"}) {
    EXPECT_FALSE(ParseJavaStatusJson("{\"description\":\"ok\"," +
                                     std::string(fields) + "}")
                     .has_value())
        << fields;
  }
}

TEST(JavaStatusJsonTest, TextAndVersionByteLimits) {
  EXPECT_TRUE(ParseJavaStatusJson(
                  WithDescription(base::Value(std::string(kMaxTextBytes, 'x'))))
                  .has_value());
  EXPECT_FALSE(ParseJavaStatusJson(WithDescription(base::Value(
                                       std::string(kMaxTextBytes + 1, 'x'))))
                   .has_value());
  for (size_t size : {kMaxTextBytes, kMaxTextBytes + 1}) {
    auto json = base::WriteJson(
        base::DictValue()
            .Set("description", "ok")
            .Set("version",
                 base::DictValue().Set("name", std::string(size, 'v'))));
    ASSERT_TRUE(json);
    EXPECT_EQ(size == kMaxTextBytes, ParseJavaStatusJson(*json).has_value());
  }
}

TEST(JavaStatusJsonTest, TextDepthAndNodeLimits) {
  EXPECT_TRUE(ParseJavaStatusJson(WithDescription(NestedText(kMaxTextDepth)))
                  .has_value());
  EXPECT_FALSE(
      ParseJavaStatusJson(WithDescription(NestedText(kMaxTextDepth + 1)))
          .has_value());
  for (size_t count : {kMaxTextNodes - 1, kMaxTextNodes}) {
    base::ListValue list;
    for (size_t i = 0; i < count; ++i) {
      list.Append("");
    }
    EXPECT_EQ(count == kMaxTextNodes - 1,
              ParseJavaStatusJson(WithDescription(base::Value(std::move(list))))
                  .has_value());
  }
}

TEST(JavaStatusJsonTest, TranslationAndExtraCannotBypassTextBudgets) {
  auto deep_parameter = base::Value(
      base::DictValue()
          .Set("translate", "key")
          .Set("with",
               base::ListValue().Append(NestedText(kMaxTextDepth - 1))));
  EXPECT_FALSE(ParseJavaStatusJson(WithDescription(std::move(deep_parameter)))
                   .has_value());
  auto excessive_extra =
      base::Value(base::DictValue()
                      .Set("text", std::string(kMaxTextBytes, 'x'))
                      .Set("extra", base::ListValue().Append("y")));
  EXPECT_FALSE(ParseJavaStatusJson(WithDescription(std::move(excessive_extra)))
                   .has_value());
}

TEST(JavaStatusJsonTest, JsonDepthAndInputByteLimits) {
  for (size_t depth : {kMaxStatusJsonDepth, kMaxStatusJsonDepth + 1}) {
    // Root object, depth-2 arrays, and the leaf value all count toward the
    // JSONReader depth limit. This metadata is unrelated to description.
    const std::string json =
        "{\"description\":\"ok\",\"padding\":" + std::string(depth - 2, '[') +
        "0" + std::string(depth - 2, ']') + "}";
    EXPECT_EQ(depth == kMaxStatusJsonDepth,
              ParseJavaStatusJson(json).has_value());
  }
  const std::string prefix = R"({"description":"ok","padding":")";
  const std::string suffix = "\"}";
  auto json =
      prefix +
      std::string(kMaxStatusJsonBytes - prefix.size() - suffix.size(), 'x') +
      suffix;
  EXPECT_TRUE(ParseJavaStatusJson(json).has_value());
  json.insert(prefix.size(), 1, 'x');
  EXPECT_FALSE(ParseJavaStatusJson(json).has_value());
}

TEST(JavaStatusJsonTest, StrictJsonAndInvalidUtf8AreRejected) {
  for (const auto& json :
       {std::string(R"({"description":"\ud800"})"),
        std::string(R"({"description":"ok",})"),
        std::string(R"({/* comment */"description":"ok"})"),
        std::string(R"({"description":"ok"} {})"),
        std::string("{\"description\":\"") + "\xff\"}",
        std::string("{\"description\":\"") + "\xc0\xaf\"}"}) {
    EXPECT_FALSE(ParseJavaStatusJson(json).has_value());
  }
}

TEST(JavaStatusJsonTest, FaviconExtractionIsOptionalAndSummaryIsRedacted) {
  auto status = ParseJavaStatusJson(
      R"({"description":"ok","favicon":"data:image/png;base64,UNTRUSTED"})");
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ("data:image/png;base64,UNTRUSTED", status->favicon);
  EXPECT_FALSE(status->favicon_error);
  EXPECT_EQ(std::string::npos, status->json_summary.find("UNTRUSTED"));
  EXPECT_NE(std::string::npos, status->json_summary.find("attachment omitted"));
  // An invalid attachment does not discard otherwise valid server status.
  status = ParseJavaStatusJson(R"({"description":"ok","favicon":1})");
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ("ok", status->motd);
  EXPECT_FALSE(status->favicon);
  EXPECT_TRUE(status->favicon_error);
}

TEST(JavaStatusJsonTest, FaviconByteLimitDoesNotRejectStatus) {
  for (size_t size : {kMaxFaviconDataUrlBytes, kMaxFaviconDataUrlBytes + 1}) {
    auto json = base::WriteJson(base::DictValue()
                                    .Set("description", "ok")
                                    .Set("favicon", std::string(size, 'i')));
    ASSERT_TRUE(json);
    auto status = ParseJavaStatusJson(*json);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(size == kMaxFaviconDataUrlBytes, status->favicon.has_value());
    EXPECT_EQ(size != kMaxFaviconDataUrlBytes,
              status->favicon_error.has_value());
    EXPECT_LT(status->json_summary.size(), 100u);
  }
}

TEST(JavaStatusJsonTest, SummaryTruncationRespectsUtf8AndIncludesNotice) {
  std::string padding;
  for (size_t i = 0; i < 10000; ++i) {
    padding += "世界";
  }
  auto json = base::WriteJson(base::DictValue()
                                  .Set("description", "ok")
                                  .Set("padding", std::move(padding)));
  ASSERT_TRUE(json);
  auto status = ParseJavaStatusJson(*json);
  ASSERT_TRUE(status.has_value());
  EXPECT_TRUE(status->json_summary_truncated);
  EXPECT_LE(status->json_summary.size(), kMaxJsonSummaryBytes);
  EXPECT_TRUE(base::IsStringUTF8(status->json_summary));
  EXPECT_TRUE(status->json_summary.ends_with("[JSON summary truncated]"));
  auto short_status = ParseJavaStatusJson(R"({"description":"ok"})");
  ASSERT_TRUE(short_status.has_value());
  EXPECT_FALSE(short_status->json_summary_truncated);
  EXPECT_EQ(R"({"description":"ok"})", short_status->json_summary);
}

}  // namespace
}  // namespace mcje
