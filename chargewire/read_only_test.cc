// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
#include "chargewire_connection.h"
#include "test_stub_server.h"
#include <odbcabstraction/exceptions.h>
#include <odbcabstraction/platform.h>
#include <odbcabstraction/spi/statement.h>
#include <boost/json.hpp>
#include <sql.h>
#include <sqlext.h>
#include "gtest/gtest.h"

namespace driver::chargewire_odbc {
namespace {
using namespace test_stub;
using odbcabstraction::Connection;
using odbcabstraction::DriverException;
const std::string kSession = std::string(kSessions) + "/s1";

StubServer::Handler ReadOnlyServer(bool confirm = true, bool stopped = false, bool patch_confirm = true) {
  return [=](const std::string& method, const std::string& path, int) -> Answer {
    if (method == "GET" && path == "/chargewire" && stopped) {
      return {200, "application/json", R"({"protocol":"chargewire","protocol_versions":["1"],"server":{"name":"stub","version":"0"},"capabilities":[],"auth":{"grant_types":["password"]},"state":"stopped"})", {}};
    }
    if ((method == "POST" && path == kSessions) || (method == "GET" && path == kSession)) {
      return {method == "POST" ? 201 : 200, "application/json",
        std::string(R"({"session_id":"s1","options":{"read_only":)") + (confirm ? "true" : "false") + "}}", {}};
    }
    if (method == "PATCH" && path == kSession) {
      return {200, "application/json", std::string(R"({"options":{"read_only":)") + (patch_confirm ? "true" : "false") + R"(},"errors":{}})", {}};
    }
    if (method == "POST" && path == kSession + "/prepared") {
      return {201, "application/json", R"({"prepared_id":"p0","result_kind":"update"})", {}};
    }
    return Serving(method, path);
  };
}
void Connect(ChargewireConnection& c, StubServer& s, bool request = true) {
  std::vector<std::string> missing;
  c.Connect(s.Properties(request ? std::map<std::string,std::string>{{"ReadOnly", "true"}} : std::map<std::string,std::string>{}), missing);
}
uint32_t Mode(ChargewireConnection& c) { return boost::get<uint32_t>(*c.GetAttribute(Connection::ACCESS_MODE)); }
void ReadOnly(ChargewireConnection& c, bool value) {
  ASSERT_TRUE(c.SetAttribute(Connection::ACCESS_MODE, static_cast<uint32_t>(value ? SQL_MODE_READ_ONLY : SQL_MODE_READ_WRITE)));
}
TEST(ReadOnly, PropertyIsSentAtCreation) {
  StubServer s(ReadOnlyServer()); ChargewireConnection c(odbcabstraction::V_3);
  Connect(c,s);
  EXPECT_TRUE(boost::json::parse(s.Body("POST", kSessions)).at("options").at("read_only").as_bool());
  EXPECT_EQ(Mode(c), SQL_MODE_READ_ONLY);
  EXPECT_THROW(ReadOnly(c, false), DriverException);
}
TEST(ReadOnly, UnconfirmedCreationFails) {
  StubServer s(ReadOnlyServer(false)); ChargewireConnection c(odbcabstraction::V_3);
  EXPECT_THROW(Connect(c,s), DriverException);
  EXPECT_EQ(s.Count("DELETE", kSession), 1);
}
TEST(ReadOnly, PreconnectAttributeIsOneWayAndSentAtCreation) {
  StubServer s(ReadOnlyServer()); ChargewireConnection c(odbcabstraction::V_3);
  ReadOnly(c,true);
  EXPECT_THROW(ReadOnly(c,false), DriverException);
  Connect(c,s,false);
  EXPECT_TRUE(boost::json::parse(s.Body("POST", kSessions)).at("options").at("read_only").as_bool());
}
TEST(ReadOnly, LazyRequestIsConfirmedBeforeWork) {
  StubServer s(ReadOnlyServer(false,true)); ChargewireConnection c(odbcabstraction::V_3);
  Connect(c,s);
  EXPECT_EQ(s.Count("POST",kSessions),0);
  EXPECT_THROW(c.CreateStatement()->Prepare("SELECT 1"),DriverException);
  EXPECT_EQ(s.Count("POST",kSession+"/prepared"),0);
}
TEST(ReadOnly, LazyAttributeGetterConfirms) {
  StubServer s(ReadOnlyServer(true,true)); ChargewireConnection c(odbcabstraction::V_3);
  Connect(c,s,false); ReadOnly(c,true);
  EXPECT_EQ(s.Count("POST",kSessions),0);
  EXPECT_EQ(Mode(c),SQL_MODE_READ_ONLY);
  EXPECT_EQ(s.Count("POST",kSessions),1);
  EXPECT_TRUE(boost::json::parse(s.Body("POST", kSessions)).at("options").at("read_only").as_bool());
}
TEST(ReadOnly, DefaultLazyAttributeGetterDoesNotWakeServer) {
  StubServer s(ReadOnlyServer(false,true)); ChargewireConnection c(odbcabstraction::V_3);
  Connect(c,s,false);
  EXPECT_EQ(Mode(c),SQL_MODE_READ_WRITE);
  EXPECT_EQ(s.Count("POST",kSessions),0);
}
TEST(ReadOnly, IgnoredTransitionFailsClosed) {
  StubServer s(ReadOnlyServer(false,false,false)); ChargewireConnection c(odbcabstraction::V_3);
  Connect(c,s,false);
  EXPECT_THROW(ReadOnly(c,true),DriverException);
  EXPECT_EQ(boost::get<uint32_t>(*c.GetAttribute(Connection::CONNECTION_DEAD)),SQL_TRUE);
  EXPECT_THROW(c.CreateStatement()->Prepare("SELECT 1"),DriverException);
  EXPECT_EQ(s.Count("POST",kSession+"/prepared"),0);
}
TEST(ReadOnly, ConfirmedTransitionIsOneWay) {
  StubServer s(ReadOnlyServer()); ChargewireConnection c(odbcabstraction::V_3);
  Connect(c,s,false); ReadOnly(c,true);
  EXPECT_EQ(s.Count("PATCH",kSession),1);
  EXPECT_THROW(ReadOnly(c,false),DriverException);
  EXPECT_EQ(s.Count("PATCH",kSession),1);
}
TEST(ReadOnly, ExplicitRefusalKeepsOrdinarySession) {
  auto ordinary = ReadOnlyServer(false);
  StubServer s([ordinary](const std::string& method, const std::string& path, int nth) -> Answer {
    if (method == "PATCH" && path == kSession)
      return {200, "application/json", R"({"options":{"read_only":false},"errors":{"read_only":{"code":"unknown_option","detail":"not supported"}}})", {}};
    return ordinary(method,path,nth);
  });
  ChargewireConnection c(odbcabstraction::V_3); Connect(c,s,false);
  try { ReadOnly(c,true); FAIL(); } catch (const DriverException& e) { EXPECT_EQ(e.GetSqlState(),"HY092"); }
  EXPECT_EQ(Mode(c),SQL_MODE_READ_WRITE);
  EXPECT_EQ(boost::get<uint32_t>(*c.GetAttribute(Connection::CONNECTION_DEAD)),SQL_FALSE);
  EXPECT_NO_THROW(c.CreateStatement()->Prepare("SELECT 1"));
}
TEST(ReadOnly, ServerImposedModeCannotBeDisabled) {
  StubServer s(ReadOnlyServer()); ChargewireConnection c(odbcabstraction::V_3);
  Connect(c,s,false);
  EXPECT_THROW(ReadOnly(c,false),DriverException);
  EXPECT_EQ(Mode(c),SQL_MODE_READ_ONLY);
  EXPECT_EQ(s.Count("PATCH",kSession),0);
}
TEST(ReadOnly, InvalidAttributeRejected) {
  ChargewireConnection c(odbcabstraction::V_3);
  EXPECT_THROW(c.SetAttribute(Connection::ACCESS_MODE,static_cast<uint32_t>(17)),DriverException);
}
TEST(ReadOnly, InvalidPropertyRejected) {
  StubServer s(ReadOnlyServer()); ChargewireConnection c(odbcabstraction::V_3);
  std::vector<std::string> missing;
  EXPECT_THROW(c.Connect(s.Properties({{"ReadOnly","maybe"}}),missing),DriverException);
}
} // namespace
} // namespace driver::chargewire_odbc
