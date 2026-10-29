// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
// Connection strings and DSNs written before the protocol was renamed to Chargewire keep
// working: a URI with the old scheme is read as chargewire://, silently. The client library
// maps the old mount path and talks to servers from before the rename by itself. This file
// and its tests are the only places in the driver that spell the old name.
#include "driver_profile.h"
#include <boost/algorithm/string.hpp>
#include <string_view>

namespace driver::chargewire_odbc {
namespace {
constexpr std::string_view kOldScheme = "warp";
}

std::string CurrentUriSpelling(const std::string& uri) {
  const auto colon = uri.find(':');
  if (colon != kOldScheme.size() || !boost::algorithm::iequals(std::string_view(uri).substr(0, colon), kOldScheme))
    return uri;
  return "chargewire" + uri.substr(colon);
}
}
