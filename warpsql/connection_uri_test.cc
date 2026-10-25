// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
#include "driver_profile.h"
#include "warpsql_connection.h"
#include <gtest/gtest.h>
namespace driver::warpsql_odbc {
TEST(ConnectionUri, WarpDefaultsToVerifiedTls) {
  auto o = WarpSqlConnection::BuildConnectOptions({{"uri", "warp://db.example.com"}});
  EXPECT_EQ(o.host, "db.example.com"); EXPECT_EQ(o.port, 443);
  EXPECT_TRUE(o.tls.enabled); EXPECT_FALSE(o.tls.skip_verify); EXPECT_EQ(o.base_path, "/warp");
}
TEST(ConnectionUri, PlainWarpAndHttpUsePort80) {
  for (auto uri : {"warp://db.example.com?tls=false", "http://db.example.com"}) {
    auto o = WarpSqlConnection::BuildConnectOptions({{"uri", uri}});
    EXPECT_EQ(o.port, 80); EXPECT_FALSE(o.tls.enabled);
  }
}
TEST(ConnectionUri, HttpsAndCustomPath) {
  auto o = WarpSqlConnection::BuildConnectOptions({{"uri", "https://db.example.com:31409/custom/warp/"}});
  EXPECT_EQ(o.port, 31409); EXPECT_TRUE(o.tls.enabled); EXPECT_EQ(o.base_path, "/custom/warp");
}
TEST(ConnectionUri, Ipv6) {
  auto o = WarpSqlConnection::BuildConnectOptions({{"uri", "warp://[::1]:31409?tls=false"}});
  EXPECT_EQ(o.host, "::1"); EXPECT_EQ(o.port, 31409); EXPECT_FALSE(o.tls.enabled);
}
TEST(ConnectionUri, ExplicitPropertiesOverrideUri) {
  auto o = WarpSqlConnection::BuildConnectOptions({{"uri", "warp://db.example.com"}, {"HOST", "localhost"},
      {"port", "31409"}, {"useEncryption", "false"}, {"basePath", "/custom"}});
  EXPECT_EQ(o.host, "localhost"); EXPECT_EQ(o.port, 31409); EXPECT_FALSE(o.tls.enabled); EXPECT_EQ(o.base_path, "/custom");
}
TEST(ConnectionUri, RejectsMalformedAndUnsupportedInput) {
  for (auto uri : {"", "warp://", "file:///tmp/database", "warp://host:0", "warp://host:65536", "warp://host:",
       "warp://host?tls=maybe", "warp://host?tls=false&tls=true", "https://host?tls=false", "warp://host?token=secret", "warp://host#fragment"})
    EXPECT_THROW(WarpSqlConnection::BuildConnectOptions({{"uri", uri}}), odbcabstraction::DriverException) << uri;
}
TEST(ConnectionUri, CredentialsAreNeverIncludedInErrors) {
  for (auto uri : {"warp://user:secret@host", "warp://user:secret@[broken"}) {
    try { WarpSqlConnection::BuildConnectOptions({{"uri", uri}}); FAIL(); }
    catch (const odbcabstraction::DriverException& e) {
      EXPECT_EQ(e.GetMessageText().find("secret"), std::string::npos);
    }
  }
}
TEST(ConnectionUri, GenericUriKeepsItsOwnDefaultInAProductWrapper) {
  auto p = NormalizeConnectionProperties({{"uri", "warp://host?tls=false"}});
  EXPECT_EQ(p.at("port"), "80");
}
}
