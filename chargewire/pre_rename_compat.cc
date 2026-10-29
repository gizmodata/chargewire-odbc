// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
// Connection strings and DSNs written before the protocol was renamed to Chargewire keep
// working: its old URI scheme and old default mount path are read as the current ones,
// silently. This file and its tests are the only places that spell the old name.
#include "driver_profile.h"
#include <boost/algorithm/string.hpp>
#include <string_view>

namespace driver::chargewire_odbc {
namespace {
constexpr std::string_view kOldScheme = "warp";
constexpr std::string_view kOldBasePath = "/warp";
}

std::string CurrentUriSpelling(const std::string& uri) {
  const auto colon = uri.find(':');
  if (colon != kOldScheme.size() || !boost::algorithm::iequals(std::string_view(uri).substr(0, colon), kOldScheme))
    return uri;
  return "chargewire" + uri.substr(colon);
}

std::string CurrentBasePath(const std::string& path) {
  std::string_view trimmed(path);
  while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.remove_suffix(1);
  return trimmed == kOldBasePath ? std::string("/chargewire") : path;
}
}
