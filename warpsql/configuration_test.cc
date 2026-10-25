// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
#include "config/configuration.h"
#include "driver_profile.h"
#include <gtest/gtest.h>
#include <algorithm>
namespace driver::warpsql_odbc {
TEST(DsnConfiguration, AdvancedNativeOptionsRemainEditable) {
  config::Configuration c;
  c.Set("uri", "warp://localhost:31337?tls=false");
  for (auto key : {"clientCertificate", "clientKey", "queryTag", "sessionTag", "defaultSchema", "basePath", "UseWideChar"}) c.Set(key, "value");
  const auto keys = c.GetCustomKeys();
  EXPECT_EQ(keys.size(), 7);
  EXPECT_EQ(std::find(keys.begin(), keys.end(), "uri"), keys.end());
  EXPECT_NE(std::find(keys.begin(), keys.end(), "clientCertificate"), keys.end());
  EXPECT_NE(std::find(keys.begin(), keys.end(), "sessionTag"), keys.end());
}
TEST(DsnConfiguration, DefaultsUseTheProductProfile) {
  config::Configuration c;
  c.LoadDefaults();
  EXPECT_EQ(c.Get("dsn"), GetDriverProfile().product);
  EXPECT_EQ(c.Get("useEncryption"), "true");
  EXPECT_EQ(c.GetCustomKeys().size(), 0);
}
}
