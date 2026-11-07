// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
// Every connection signs in on its own: no token is shared between connections or kept for a
// process. After a person stops a GizmoData Cloud cluster, its stand-in refuses every token
// issued before the Stop (invalid_token, token_revoked); a new connection must still sign in
// for itself (which starts the cluster), while an old one is reported dead and never signs in
// again by itself.

#include "chargewire_connection.h"

#include <odbcabstraction/exceptions.h>
#include <odbcabstraction/platform.h>
#include <odbcabstraction/spi/statement.h>
#include <sql.h>

#include <atomic>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test_stub_server.h"

namespace driver::chargewire_odbc {
namespace {

using namespace test_stub;
using odbcabstraction::Connection;

const std::string kPrepared = std::string(kSessions) + "/s1/prepared";

// Issues token t<n> to the n-th sign-in; once `stopped` is set, refuses the tokens issued
// before it (token_revoked), as GizmoData Cloud does after a person's Stop.
struct Server {
  std::atomic<int> revoked_below{0};  // tokens t0 .. t<revoked_below - 1> are refused
  StubServer stub{[this](const std::string& method, const std::string& path, int nth) -> Answer {
    if (method == "POST" && path == kToken) {
      return {200, "application/json", R"({"access_token":"t)" + std::to_string(nth) + R"(","expires_in":3600})", {}};
    }
    const std::string& authorization = StubServer::CurrentAuthorization();
    if (authorization.rfind("Bearer t", 0) == 0 && std::stoi(authorization.substr(8)) < revoked_below) {
      return {401, "application/problem+json",
              R"({"status":401,"code":"invalid_token","reason":"token_revoked","detail":"revoked by a Stop"})",
              {{"WWW-Authenticate", "Bearer"}}};
    }
    if (method == "POST" && path == kPrepared) {
      return {201, "application/json", R"({"prepared_id":"p0","result_kind":"update"})", {}};
    }
    return Serving(method, path);
  }};
};

bool Dead(ChargewireConnection& connection) {
  return boost::get<uint32_t>(*connection.GetAttribute(Connection::CONNECTION_DEAD)) == SQL_TRUE;
}

TEST(OwnSignIn, EachConnectionSignsInForItselfAndANewOneWorksAfterAStop) {
  Server server;
  ChargewireConnection old_connection(odbcabstraction::V_3);
  std::vector<std::string> missing;
  old_connection.Connect(server.stub.Properties(), missing);
  auto old_statement = old_connection.CreateStatement();
  old_statement->Prepare("SELECT 1");
  EXPECT_EQ(server.stub.Count("POST", kToken), 1);

  server.revoked_below = 1;  // a person's Stop: t0 no longer stands

  // A new connection in the same process signs in itself (t1), and its first request works.
  ChargewireConnection new_connection(odbcabstraction::V_3);
  new_connection.Connect(server.stub.Properties(), missing);
  EXPECT_EQ(server.stub.Count("POST", kToken), 2);
  auto new_statement = new_connection.CreateStatement();
  EXPECT_NO_THROW(new_statement->Prepare("SELECT 1"));
  EXPECT_FALSE(Dead(new_connection));

  // The old connection is refused, reported dead, and does not sign in again by itself.
  try {
    old_statement->Prepare("SELECT 2");
    FAIL() << "a revoked token ran a statement";
  } catch (const odbcabstraction::DriverException& e) {
    EXPECT_EQ(e.GetSqlState(), "28000") << e.GetMessageText();
  }
  EXPECT_TRUE(Dead(old_connection));
  EXPECT_EQ(server.stub.Count("POST", kToken), 2);

  new_statement.reset();
  old_statement.reset();
  new_connection.Close();
  old_connection.Close();
  EXPECT_EQ(server.stub.Count("POST", kToken), 2);  // neither close signed in
}

}  // namespace
}  // namespace driver::chargewire_odbc
