// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcje/java_status_json.h"

#include <cmath>
#include <limits>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_util.h"
#include "base/values.h"

namespace mcje {
namespace {

class TextFlattener {
 public:
  bool Visit(const base::Value& value, size_t depth = 1) {
    if (depth > kMaxTextDepth || ++nodes_ > kMaxTextNodes) {
      return false;
    }
    if (const auto* text = value.GetIfString()) {
      return Append(*text);
    }
    if (const auto* list = value.GetIfList()) {
      for (const auto& child : *list) {
        if (!Visit(child, depth + 1)) {
          return false;
        }
      }
      return true;
    }
    const auto* object = value.GetIfDict();
    if (!object) {
      return false;
    }
    const auto* text = object->Find("text");
    const auto* translate = object->Find("translate");
    const auto* fallback = object->Find("fallback");
    const auto* parameters = object->Find("with");
    const auto* extra = object->Find("extra");
    if ((text && !text->is_string()) ||
        (translate && !translate->is_string()) ||
        (fallback && !fallback->is_string()) ||
        (parameters && !parameters->is_list()) ||
        (extra && !extra->is_list())) {
      return false;
    }
    if (text) {
      if (!Append(text->GetString())) {
        return false;
      }
    } else if (translate) {
      if (!Append((fallback ? fallback : translate)->GetString())) {
        return false;
      }
      // No language resources or format-string interpreter in the first
      // version. Preserve arguments explicitly alongside the fallback/key.
      if (parameters && !parameters->GetList().empty()) {
        if (depth + 1 > kMaxTextDepth || ++nodes_ > kMaxTextNodes ||
            !Append(" [")) {
          return false;
        }
        bool first = true;
        for (const auto& parameter : parameters->GetList()) {
          if ((!first && !Append(", ")) || !Visit(parameter, depth + 2)) {
            return false;
          }
          first = false;
        }
        if (!Append("]")) {
          return false;
        }
      }
    } else if (!extra) {
      return false;
    }
    // Style, clickEvent, hoverEvent and other metadata are never evaluated.
    return !extra || Visit(*extra, depth + 1);
  }

  std::string TakeText() {
    std::string plain;
    for (size_t i = 0; i < text_.size();) {
      if (std::string_view(text_).substr(i, 2) == "\xc2\xa7" &&
          i + 2 < text_.size() &&
          std::string_view("0123456789abcdefklmnor")
                  .find(base::ToLowerASCII(text_[i + 2])) !=
              std::string_view::npos) {
        i += 3;
      } else {
        plain += text_[i++];
      }
    }
    return plain;
  }

 private:
  bool Append(std::string_view text) {
    if (text.size() > kMaxTextBytes - text_.size()) {
      return false;
    }
    text_.append(text);
    return true;
  }

  size_t nodes_ = 0;
  std::string text_;
};

bool ReadOptionalInteger(const base::DictValue& object,
                         std::string_view key,
                         int minimum,
                         std::optional<int>& output) {
  const auto* value = object.Find(key);
  if (!value) {
    return true;
  }
  const auto number = value->GetIfDouble();
  if (!number || !std::isfinite(*number) || std::trunc(*number) != *number ||
      *number < minimum || *number > std::numeric_limits<int>::max()) {
    return false;
  }
  output = static_cast<int>(*number);
  return true;
}

}  // namespace

base::expected<JavaServerStatus, std::string> ParseJavaStatusJson(
    std::string_view json) {
  if (json.size() > kMaxStatusJsonBytes ||
      !base::IsStringUTF8AllowingNoncharacters(json)) {
    return base::unexpected(
        "Status JSON exceeds the input limit or is not UTF-8.");
  }
  auto root =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC, kMaxStatusJsonDepth);
  if (!root || !root->is_dict()) {
    return base::unexpected(
        "Status JSON must be an object within the depth limit.");
  }
  auto& object = root->GetDict();
  const auto* description = object.Find("description");
  TextFlattener flattener;
  if (!description || !flattener.Visit(*description)) {
    return base::unexpected("Invalid or excessive description text component.");
  }
  JavaServerStatus status;
  status.motd = flattener.TakeText();
  if (const auto* version_value = object.Find("version")) {
    const auto* version = version_value->GetIfDict();
    if (!version || !ReadOptionalInteger(*version, "protocol",
                                         std::numeric_limits<int>::min(),
                                         status.protocol_version)) {
      return base::unexpected("Invalid version object or protocol number.");
    }
    if (const auto* name = version->Find("name")) {
      if (!name->is_string() || name->GetString().size() > kMaxTextBytes) {
        return base::unexpected("Invalid or excessive version name.");
      }
      status.version_name = name->GetString();
    }
  }
  if (const auto* players_value = object.Find("players")) {
    const auto* players = players_value->GetIfDict();
    if (!players ||
        !ReadOptionalInteger(*players, "online", 0, status.players_online) ||
        !ReadOptionalInteger(*players, "max", 0, status.players_max)) {
      return base::unexpected("Invalid players object or player count.");
    }
  }
  if (const auto* favicon = object.Find("favicon")) {
    if (!favicon->is_string()) {
      status.favicon_error = "Favicon attachment must be a string.";
    } else if (favicon->GetString().size() > kMaxFaviconDataUrlBytes) {
      status.favicon_error = "Favicon attachment exceeds the input limit.";
    } else {
      status.favicon = favicon->GetString();
    }
    object.Set("favicon", "[favicon attachment omitted]");
  }
  auto summary = base::WriteJson(*root, kMaxStatusJsonDepth);
  if (!summary) {
    return base::unexpected("Could not serialize the status JSON summary.");
  }
  if (summary->size() > kMaxJsonSummaryBytes) {
    constexpr std::string_view kNotice = "\n[JSON summary truncated]";
    status.json_summary = base::TruncateUTF8ToByteSize(
        *summary, kMaxJsonSummaryBytes - kNotice.size());
    status.json_summary += kNotice;
    status.json_summary_truncated = true;
  } else {
    status.json_summary = std::move(*summary);
  }
  return status;
}

}  // namespace mcje
