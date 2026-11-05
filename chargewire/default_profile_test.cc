// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
#include "driver_profile.h"
#include "chargewire_connection.h"
#include <gtest/gtest.h>
namespace driver::chargewire_odbc {
// The generic driver gives every host the same default port; only a wrapper's profile varies it.
TEST(DefaultProfile, SameDefaultPortForEveryHost) {
  EXPECT_EQ(GetDriverProfile().default_port_for_host, nullptr);
  for (auto host : {"localhost", "db.example.com", "203.0.113.7", "::1"}) {
    EXPECT_EQ(DefaultPortForHost(GetDriverProfile(), host), 443) << host;
    EXPECT_EQ(ChargewireConnection::BuildConnectOptions({{"host", host}}).port, 443) << host;
  }
}
}
