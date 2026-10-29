// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
#include "driver_profile.h"
#include "chargewire_connection.h"
#include <gtest/gtest.h>
namespace driver::chargewire_odbc {
TEST(ConnectionUri, ChargewireDefaultsToVerifiedTls) {
  auto o = ChargewireConnection::BuildConnectOptions({{"uri", "chargewire://db.example.com"}});
  EXPECT_EQ(o.host, "db.example.com"); EXPECT_EQ(o.port, 443);
  EXPECT_TRUE(o.tls.enabled); EXPECT_FALSE(o.tls.skip_verify); EXPECT_EQ(o.base_path, "/chargewire");
}
TEST(ConnectionUri, PlainChargewireAndHttpUsePort80) {
  for (auto uri : {"chargewire://db.example.com?tls=false", "http://db.example.com"}) {
    auto o = ChargewireConnection::BuildConnectOptions({{"uri", uri}});
    EXPECT_EQ(o.port, 80); EXPECT_FALSE(o.tls.enabled);
  }
}
TEST(ConnectionUri, HttpsAndCustomPath) {
  auto o = ChargewireConnection::BuildConnectOptions({{"uri", "https://db.example.com:31409/custom/chargewire/"}});
  EXPECT_EQ(o.port, 31409); EXPECT_TRUE(o.tls.enabled); EXPECT_EQ(o.base_path, "/custom/chargewire");
}
TEST(ConnectionUri, Ipv6) {
  auto o = ChargewireConnection::BuildConnectOptions({{"uri", "chargewire://[::1]:31409?tls=false"}});
  EXPECT_EQ(o.host, "::1"); EXPECT_EQ(o.port, 31409); EXPECT_FALSE(o.tls.enabled);
}
TEST(ConnectionUri, ExplicitPropertiesOverrideUri) {
  auto o = ChargewireConnection::BuildConnectOptions({{"uri", "chargewire://db.example.com"}, {"HOST", "localhost"},
      {"port", "31409"}, {"useEncryption", "false"}, {"basePath", "/custom"}});
  EXPECT_EQ(o.host, "localhost"); EXPECT_EQ(o.port, 31409); EXPECT_FALSE(o.tls.enabled); EXPECT_EQ(o.base_path, "/custom");
}
TEST(ConnectionUri, RejectsMalformedAndUnsupportedInput) {
  for (auto uri : {"", "chargewire://", "file:///tmp/database", "chargewire://host:0", "chargewire://host:65536", "chargewire://host:",
       "chargewire://host?tls=maybe", "chargewire://host?tls=false&tls=true", "https://host?tls=false", "chargewire://host?token=secret", "chargewire://host#fragment"})
    EXPECT_THROW(ChargewireConnection::BuildConnectOptions({{"uri", uri}}), odbcabstraction::DriverException) << uri;
}
TEST(ConnectionUri, CredentialsAreNeverIncludedInErrors) {
  for (auto uri : {"chargewire://user:secret@host", "chargewire://user:secret@[broken"}) {
    try { ChargewireConnection::BuildConnectOptions({{"uri", uri}}); FAIL(); }
    catch (const odbcabstraction::DriverException& e) {
      EXPECT_EQ(e.GetMessageText().find("secret"), std::string::npos);
    }
  }
}
TEST(ConnectionUri, GenericUriKeepsItsOwnDefaultInAProductWrapper) {
  auto p = NormalizeConnectionProperties({{"uri", "chargewire://host?tls=false"}});
  EXPECT_EQ(p.at("port"), "80");
}
}
