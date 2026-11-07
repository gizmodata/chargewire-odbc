// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
// Lazy connect to a stopped server (Chargewire spec §3.1 `state`, §11.2). A pool opens and
// checks connections on its own schedule, and a sign-in at a stand-in for a stopped server
// starts the server. So connecting to a stopped server signs in to nothing and opens no
// session; checks, SQLGetInfo and closing send nothing; the first statement signs in.

#include "chargewire_connection.h"

#include <odbcabstraction/exceptions.h>
#include <odbcabstraction/platform.h>
#include <odbcabstraction/spi/statement.h>
#include <sql.h>
#include <sqlext.h>

#include <map>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test_stub_server.h"

namespace driver::chargewire_odbc {
namespace {

using namespace test_stub;
using odbcabstraction::Connection;

const std::string kRoot = "/chargewire";
const std::string kPrepared = std::string(kSessions) + "/s1/prepared";

// A stand-in for a stopped server (`stopped`), or a server that serves: the first `resuming`
// sign-ins are answered server_resuming, and a password other than "pw" is refused.
StubServer::Handler Stand(bool stopped, int resuming = 0) {
  return [stopped, resuming](const std::string& method, const std::string& path, int nth) -> Answer {
    if (method == "GET" && path == kRoot) {
      return {200, "application/json",
              std::string(R"({"protocol":"chargewire","protocol_versions":["1"],"server":{"name":"stub","version":"0"},)") +
                  R"("capabilities":["prepared_statements"],"auth":{"grant_types":["password"]})" +
                  (stopped ? R"(,"state":"stopped"})" : "}"),
              {}};
    }
    if (method == "POST" && path == kToken && nth < resuming) return Resuming("1");
    if (method == "POST" && path == kPrepared) {
      return {201, "application/json", R"({"prepared_id":"p0","result_kind":"update"})", {}};
    }
    if (method == "GET" && path == std::string(kSessions) + "/s1/metadata/server-info") {
      return {200, "application/json", R"({"sql_identifier_quote_char":"\""})", {}};
    }
    return Serving(method, path);
  };
}

// A stub that refuses the password: every sign-in answers 401 invalid_credentials.
StubServer::Handler Refusing(bool stopped) {
  auto stand = Stand(stopped);
  return [stand](const std::string& method, const std::string& path, int nth) -> Answer {
    if (method == "POST" && path == kToken) {
      return {401, "application/problem+json",
              R"({"status":401,"code":"invalid_credentials","detail":"wrong user name or password"})", {}};
    }
    return stand(method, path, nth);
  };
}

bool Dead(ChargewireConnection& connection) {
  return boost::get<uint32_t>(*connection.GetAttribute(Connection::CONNECTION_DEAD)) == SQL_TRUE;
}

void Connect(ChargewireConnection& connection, StubServer& stub) {
  std::vector<std::string> missing;
  connection.Connect(stub.Properties(), missing);
}

TEST(LazyConnect, ConnectingToAStoppedServerSendsOnlyDiscovery) {
  StubServer stub(Stand(/*stopped=*/true));
  ChargewireConnection connection(odbcabstraction::V_3);
  Connect(connection, stub);
  EXPECT_EQ(stub.Seen(), std::vector<std::string>{"GET " + kRoot});
}

// What a driver manager and a pool do with a fresh connection: check it, ask SQLGetInfo, close it.
TEST(LazyConnect, CheckingAskingInfoAndClosingSendNothing) {
  StubServer stub(Stand(/*stopped=*/true));
  ChargewireConnection connection(odbcabstraction::V_3);
  Connect(connection, stub);
  EXPECT_FALSE(Dead(connection));
  EXPECT_EQ(boost::get<std::string>(connection.GetInfo(SQL_DBMS_NAME)), GetDriverProfile().product);
  EXPECT_NO_THROW(connection.GetInfo(SQL_DRIVER_ODBC_VER));
  EXPECT_NO_THROW(connection.GetInfo(SQL_IDENTIFIER_QUOTE_CHAR));
  auto statement = connection.CreateStatement();
  statement->Cancel();
  statement.reset();
  connection.BeginClose();
  connection.Close();
  EXPECT_EQ(stub.Seen(), std::vector<std::string>{"GET " + kRoot});
}

TEST(LazyConnect, TheFirstStatementSignsInOpensTheSessionAndRuns) {
  StubServer stub(Stand(/*stopped=*/true, /*resuming=*/1));
  ChargewireConnection connection(odbcabstraction::V_3);
  Connect(connection, stub);
  EXPECT_FALSE(Dead(connection));
  ASSERT_EQ(stub.Seen(), std::vector<std::string>{"GET " + kRoot});  // nothing yet
  auto statement = connection.CreateStatement();
  statement->Prepare("SELECT 1");  // through one server_resuming wait
  EXPECT_EQ(stub.Seen(), (std::vector<std::string>{"GET " + kRoot, std::string("POST ") + kToken,
                                                   std::string("POST ") + kToken, std::string("POST ") + kSessions,
                                                   "POST " + kPrepared}));
  EXPECT_FALSE(Dead(connection));
  // Now the server answers SQLGetInfo, as before.
  EXPECT_EQ(boost::get<std::string>(connection.GetInfo(SQL_DBMS_NAME)), "stub");
  statement.reset();
  connection.Close();
}

TEST(LazyConnect, AWrongPasswordIsReportedByTheFirstStatement) {
  StubServer stub(Refusing(/*stopped=*/true));
  ChargewireConnection connection(odbcabstraction::V_3);
  Connect(connection, stub);
  auto statement = connection.CreateStatement();
  try {
    statement->Prepare("SELECT 1");
    FAIL() << "prepared with a wrong password";
  } catch (const odbcabstraction::DriverException& e) {
    EXPECT_EQ(e.GetSqlState(), "28000") << e.GetMessageText();
    EXPECT_NE(e.GetMessageText().find("wrong user name or password"), std::string::npos) << e.GetMessageText();
  }
  EXPECT_EQ(stub.Count("POST", kSessions), 0);
  statement.reset();
  connection.Close();
}

// The positive twins: a server that serves is signed in to at connect, and a wrong password
// fails the connect, as before.
TEST(LazyConnect, AServerThatServesIsSignedInToAtConnectAsBefore) {
  StubServer stub(Stand(/*stopped=*/false));
  ChargewireConnection connection(odbcabstraction::V_3);
  Connect(connection, stub);
  EXPECT_EQ(stub.Seen(), (std::vector<std::string>{"GET " + kRoot, std::string("POST ") + kToken,
                                                   std::string("POST ") + kSessions}));
  connection.Close();

  StubServer refusing(Refusing(/*stopped=*/false));
  ChargewireConnection refused(odbcabstraction::V_3);
  std::vector<std::string> missing;
  try {
    refused.Connect(refusing.Properties(), missing);
    FAIL() << "connected with a wrong password";
  } catch (const odbcabstraction::DriverException& e) {
    EXPECT_EQ(e.GetSqlState(), "28000") << e.GetMessageText();
  }
}

}  // namespace
}  // namespace driver::chargewire_odbc
