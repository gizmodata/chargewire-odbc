// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
// SQL_ATTR_CONNECTION_DEAD, which driver managers' pools ask before handing a connection out:
// asking sends nothing (no sign-in, no session), and a connection is dead once the server
// refused its token for good (spec §4.2) or lost its session (spec §5.6).

#include "chargewire_connection.h"

#include <odbcabstraction/exceptions.h>
#include <odbcabstraction/platform.h>
#include <odbcabstraction/spi/statement.h>
#include <sql.h>

#include <chrono>
#include <map>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "test_stub_server.h"

namespace driver::chargewire_odbc {
namespace {

using namespace std::chrono_literals;
using namespace test_stub;
using odbcabstraction::Connection;

const std::string kPrepared = std::string(kSessions) + "/s1/prepared";

Answer Refused(const std::string& code, const std::string& reason) {
  std::string body = R"({"status":401,"code":")" + code + R"(","detail":"from the stub")";
  if (!reason.empty()) body += R"(,"reason":")" + reason + R"(")";
  return {401, "application/problem+json", body + "}", {{"WWW-Authenticate", "Bearer"}}};
}

bool Dead(ChargewireConnection& connection) {
  return boost::get<uint32_t>(*connection.GetAttribute(Connection::CONNECTION_DEAD)) == SQL_TRUE;
}

// Connects to a stub that refuses every prepare with `refusal`, and prepares once.
bool DeadAfter(const Answer& refusal, const std::map<std::string, std::string>& extra = {}) {
  StubServer stub([&](const std::string& method, const std::string& path, int) {
    if (method == "POST" && path == kPrepared) return refusal;
    return Serving(method, path);
  });
  ChargewireConnection connection(odbcabstraction::V_3);
  std::vector<std::string> missing;
  connection.Connect(stub.Properties(extra), missing);
  EXPECT_FALSE(Dead(connection));
  auto statement = connection.CreateStatement();
  try {
    statement->Prepare("SELECT 1");
    ADD_FAILURE() << "prepared";
  } catch (const odbcabstraction::DriverException& e) {
    EXPECT_EQ(e.GetSqlState(), "28000") << e.GetMessageText();
  }
  const bool dead = Dead(connection);
  statement.reset();
  connection.Close();
  return dead;
}

TEST(ConnectionLiveness, AskingSendsNothing) {
  // Tokens past the client library's renew point a moment after sign-in: a check that renewed
  // or opened a session would show.
  StubServer stub([](const std::string& method, const std::string& path, int) { return Serving(method, path, 61); });
  ChargewireConnection connection(odbcabstraction::V_3);
  std::vector<std::string> missing;
  connection.Connect(stub.Properties(), missing);
  std::this_thread::sleep_for(1500ms);
  const auto seen = stub.Seen().size();
  for (int i = 0; i < 3; i++) EXPECT_FALSE(Dead(connection));
  EXPECT_EQ(stub.Seen().size(), seen);
  connection.Close();
  EXPECT_TRUE(Dead(connection));
}

TEST(ConnectionLiveness, ATokenRefusedForGoodMakesTheConnectionDead) {
  for (const char* reason : {"token_revoked", "credentials_changed", "principal_dropped"}) {
    SCOPED_TRACE(reason);
    EXPECT_TRUE(DeadAfter(Refused("invalid_token", reason)));
  }
}

// An access token from the connection string is not renewed, so its expiry is final; a password
// sign-in's token is renewed on the next request, so the connection stays usable.
TEST(ConnectionLiveness, AnExpiredTokenIsDeadOnlyWhenTheDriverCannotRenewIt) {
  EXPECT_TRUE(DeadAfter(Refused("invalid_token", "token_expired"), {{"authType", "token"}, {"token", "given"}}));
  EXPECT_FALSE(DeadAfter(Refused("invalid_token", "token_expired")));
}

// Without a reason (a token the server can't call its own) the connection stays as it was.
TEST(ConnectionLiveness, ARefusalWithoutAReasonLeavesTheConnectionAlive) {
  EXPECT_FALSE(DeadAfter(Refused("invalid_token", "")));
}

}  // namespace
}  // namespace driver::chargewire_odbc
