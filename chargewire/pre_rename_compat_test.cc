// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
// Connection settings written before the rename to Chargewire (pre_rename_compat.cc).
#include "driver_profile.h"
#include "chargewire_connection.h"
#include <gtest/gtest.h>
namespace driver::chargewire_odbc {
namespace {
void ExpectSameOptions(const ConnectionProperties& old_style, const ConnectionProperties& current) {
  const auto a = ChargewireConnection::BuildConnectOptions(old_style);
  const auto b = ChargewireConnection::BuildConnectOptions(current);
  EXPECT_EQ(a.host, b.host); EXPECT_EQ(a.port, b.port); EXPECT_EQ(a.base_path, b.base_path);
  EXPECT_EQ(a.tls.enabled, b.tls.enabled); EXPECT_EQ(a.tls.skip_verify, b.tls.skip_verify);
}
}
TEST(PreRenameCompat, OldSchemeReadsAsCurrent) {
  ExpectSameOptions({{"uri", "warp://db.example.com"}}, {{"uri", "chargewire://db.example.com"}});
  ExpectSameOptions({{"uri", "WARP://db.example.com:31337?tls=false"}}, {{"uri", "chargewire://db.example.com:31337?tls=false"}});
  ExpectSameOptions({{"uri", "Warp://[::1]:31409?tls=false"}}, {{"uri", "chargewire://[::1]:31409?tls=false"}});
  auto o = ChargewireConnection::BuildConnectOptions({{"uri", "warp://db.example.com"}});
  EXPECT_EQ(o.base_path, "/chargewire"); EXPECT_EQ(o.port, 443); EXPECT_TRUE(o.tls.enabled);
}
TEST(PreRenameCompat, OldDefaultMountReadsAsCurrent) {
  EXPECT_EQ(ChargewireConnection::BuildConnectOptions({{"uri", "https://db.example.com/warp"}}).base_path, "/chargewire");
  EXPECT_EQ(ChargewireConnection::BuildConnectOptions({{"uri", "http://db.example.com/warp/"}}).base_path, "/chargewire");
  EXPECT_EQ(ChargewireConnection::BuildConnectOptions({{"host", "h"}, {"basePath", "/warp"}}).base_path, "/chargewire");
  EXPECT_EQ(ChargewireConnection::BuildConnectOptions({{"uri", "warp://h"}, {"BASEPATH", "/warp/"}}).base_path, "/chargewire");
}
TEST(PreRenameCompat, OtherSettingsAreUntouched) {
  EXPECT_EQ(CurrentUriSpelling("chargewire://h"), "chargewire://h");
  EXPECT_EQ(CurrentUriSpelling("https://warp.example.com/warp"), "https://warp.example.com/warp");
  EXPECT_EQ(CurrentUriSpelling("warpdrive://h"), "warpdrive://h");
  EXPECT_EQ(CurrentUriSpelling("warp"), "warp");
  EXPECT_EQ(CurrentBasePath("/custom/warp"), "/custom/warp");
  EXPECT_EQ(CurrentBasePath("/Warp"), "/Warp");
  EXPECT_EQ(CurrentBasePath("/warpspeed"), "/warpspeed");
  EXPECT_EQ(ChargewireConnection::BuildConnectOptions({{"uri", "https://h/custom/warp"}}).base_path, "/custom/warp");
}
TEST(PreRenameCompat, OldSchemeKeepsItsValidationAndSecrecy) {
  for (auto uri : {"warp://", "warp://host:0", "warp://host?tls=maybe", "warp://host?token=secret", "warp://host#fragment"})
    EXPECT_THROW(ChargewireConnection::BuildConnectOptions({{"uri", uri}}), odbcabstraction::DriverException) << uri;
  try { ChargewireConnection::BuildConnectOptions({{"uri", "warp://user:secret@host"}}); FAIL(); }
  catch (const odbcabstraction::DriverException& e) {
    EXPECT_EQ(e.GetMessageText().find("secret"), std::string::npos);
  }
}
}
