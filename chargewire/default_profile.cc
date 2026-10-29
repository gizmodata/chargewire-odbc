// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
#include "driver_profile.h"
namespace driver::chargewire_odbc {
const DriverProfile& GetDriverProfile() {
  static const DriverProfile profile{"Chargewire ODBC Driver", "Chargewire",
      "chargewire-odbc/" CHARGEWIRE_ODBC_VERSION, "chargewire-odbc.ini", 443};
  return profile;
}
std::string TranslateProductUri(const std::string& uri) { return uri; }
void InitializeProductSession(ChargewireClient&, const ConnectionProperties& properties, const CallOptions&) {
  if (properties.find("sessionTag") != properties.end())
    throw odbcabstraction::DriverException("sessionTag is a product-specific option; use the Hardcharger wrapper", "HY024");
}
}
