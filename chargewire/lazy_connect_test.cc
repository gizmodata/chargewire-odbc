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
#include "utils.h"
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

// A product profile that sets a session option as the session opens (as Hardcharger's sets its
// query and session tags with SET SESSION).
class TaggingConnection : public ChargewireConnection {
 public:
  TaggingConnection() : ChargewireConnection(odbcabstraction::V_3) {}
 protected:
  void InitializeSession(ChargewireClient& client, const ConnPropertyMap&, const CallOptions& options) override {
    ThrowIfNotOK(client.ExecuteUpdate(options, "SET SESSION product.tag = 'x'").status());
  }
};

const std::string kStatements = std::string(kSessions) + "/s1/statements";

// Stand(), with the SET answered `set` (a statement document by default).
StubServer::Handler WithSet(bool stopped, Answer set = {200, "application/json",
    R"({"statement_id":"st1","state":"succeeded","result":{"kind":"update","rows_affected":0}})", {}}) {
  auto stand = Stand(stopped);
  return [stand, set](const std::string& method, const std::string& path, int nth) -> Answer {
    if (method == "POST" && path == kStatements) return set;
    if (method == "PATCH" && path == std::string(kSessions) + "/s1") {
      return {200, "application/json", R"({"options":{"autocommit":false},"errors":{}})", {}};
    }
    return stand(method, path, nth);
  };
}

const Answer kSetRefused{400, "application/problem+json",
    R"({"status":400,"code":"sql_error","sqlstate":"42704","detail":"unrecognized configuration parameter"})", {}};

TEST(LazyConnect, AProductsSessionSettingsWaitForTheFirstStatement) {
  StubServer stub(WithSet(/*stopped=*/true));
  TaggingConnection connection;
  Connect(connection, stub);
  EXPECT_EQ(stub.Seen(), std::vector<std::string>{"GET " + kRoot});
  auto statement = connection.CreateStatement();
  statement->Prepare("SELECT 1");
  EXPECT_EQ(stub.Seen(), (std::vector<std::string>{"GET " + kRoot, std::string("POST ") + kToken,
                                                   std::string("POST ") + kSessions, "POST " + kStatements,
                                                   "POST " + kPrepared}));
  statement->Prepare("SELECT 2");  // once only
  EXPECT_EQ(stub.Count("POST", kStatements), 1);
  statement.reset();
  connection.Close();
}

// A refused setting fails the first statement with the SQLSTATE a connect to a server that
// serves fails with, and every statement after it until it is set.
TEST(LazyConnect, ARefusedSessionSettingFailsTheFirstStatementAsItFailsAConnect) {
  StubServer serving(WithSet(/*stopped=*/false, kSetRefused));
  TaggingConnection eager;
  std::string at_connect;
  try {
    Connect(eager, serving);
    FAIL() << "connected";
  } catch (const odbcabstraction::DriverException& e) {
    at_connect = e.GetSqlState();
  }
  EXPECT_EQ(at_connect, "42704");

  StubServer stub(WithSet(/*stopped=*/true, kSetRefused));
  TaggingConnection connection;
  Connect(connection, stub);
  auto statement = connection.CreateStatement();
  for (int i = 0; i < 2; i++) {
    try {
      statement->Prepare("SELECT 1");
      FAIL() << "prepared without the session setting";
    } catch (const odbcabstraction::DriverException& e) {
      EXPECT_EQ(e.GetSqlState(), at_connect) << e.GetMessageText();
    }
  }
  EXPECT_EQ(stub.Count("POST", kPrepared), 0);
  statement.reset();
  connection.Close();
}

TEST(LazyConnect, AProductsSessionSettingsGoOnAtConnectToAServerThatServes) {
  StubServer stub(WithSet(/*stopped=*/false));
  TaggingConnection connection;
  Connect(connection, stub);
  EXPECT_EQ(stub.Seen(), (std::vector<std::string>{"GET " + kRoot, std::string("POST ") + kToken,
                                                   std::string("POST ") + kSessions, "POST " + kStatements}));
  auto statement = connection.CreateStatement();
  statement->Prepare("SELECT 1");
  EXPECT_EQ(stub.Count("POST", kStatements), 1);
  statement.reset();
  connection.Close();
}

// Transactions before the first request that does work (spec §11.2): turning autocommit off and
// ending a transaction are not work, and send nothing.
TEST(LazyConnect, AutocommitOffAndEndingATransactionSendNothing) {
  StubServer stub(Stand(/*stopped=*/true));
  ChargewireConnection connection(odbcabstraction::V_3);
  Connect(connection, stub);
  connection.SetAutoCommit(false);  // SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_OFF)
  connection.EndTransaction(true);  // SQLEndTran(SQL_COMMIT)
  connection.EndTransaction(false);  // SQLEndTran(SQL_ROLLBACK)
  connection.SetAutoCommit(true);
  connection.SetAutoCommit(false);
  EXPECT_FALSE(Dead(connection));
  connection.BeginClose();
  connection.Close();  // SQLDisconnect
  EXPECT_EQ(stub.Seen(), std::vector<std::string>{"GET " + kRoot});
}

// Autocommit off, set before the first statement, goes with the session's creation: the
// statement runs in a transaction, with no separate request to turn autocommit off.
TEST(LazyConnect, AutocommitOffGoesWithTheSessionsCreation) {
  StubServer stub(Stand(/*stopped=*/true));
  ChargewireConnection connection(odbcabstraction::V_3);
  Connect(connection, stub);
  connection.SetAutoCommit(false);
  EXPECT_EQ(stub.Seen(), std::vector<std::string>{"GET " + kRoot});
  auto statement = connection.CreateStatement();
  statement->Prepare("SELECT 1");
  EXPECT_EQ(stub.Seen(), (std::vector<std::string>{"GET " + kRoot, std::string("POST ") + kToken,
                                                   std::string("POST ") + kSessions, "POST " + kPrepared}));
  const auto created = boost::json::parse(stub.Body("POST", kSessions)).as_object();
  EXPECT_EQ(created.at("options").as_object().at("autocommit"), false) << stub.Body("POST", kSessions);
  EXPECT_TRUE(connection.GetCurrentTransaction().is_valid());
  statement.reset();
  connection.Close();
}

// With a product's session settings, the settings go on as the session opens and autocommit is
// turned off right after, both before the first statement runs (spec §11.2's "or right after it").
TEST(LazyConnect, AutocommitOffWithAProductsSessionSettings) {
  StubServer stub(WithSet(/*stopped=*/true));
  TaggingConnection connection;
  Connect(connection, stub);
  connection.SetAutoCommit(false);
  connection.EndTransaction(true);
  EXPECT_EQ(stub.Seen(), std::vector<std::string>{"GET " + kRoot});
  auto statement = connection.CreateStatement();
  statement->Prepare("SELECT 1");
  EXPECT_EQ(stub.Seen(), (std::vector<std::string>{"GET " + kRoot, std::string("POST ") + kToken,
                                                   std::string("POST ") + kSessions, "POST " + kStatements,
                                                   "PATCH " + std::string(kSessions) + "/s1", "POST " + kPrepared}));
  statement.reset();
  connection.Close();
}

}  // namespace
}  // namespace driver::chargewire_odbc
