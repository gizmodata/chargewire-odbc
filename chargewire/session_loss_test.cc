// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
// A session the server ended or forgot (spec §5.6): the diagnostic and the dead connection.
#include "chargewire_client.h"

#include <odbcabstraction/exceptions.h>

#include "gtest/gtest.h"

namespace driver::chargewire_odbc {
namespace {

chargewire::client::Error Problem(int status, const std::string& code, const std::string& reason = "") {
  boost::json::object problem{{"code", code}, {"detail", "detail from the server"}};
  if (!reason.empty()) problem["reason"] = reason;
  return chargewire::client::Error::FromProblem(status, problem);
}

// The DriverException ThrowChargewireErrors turns `error` into, and whether it marked the
// session lost.
std::pair<odbcabstraction::DriverException, bool> Map(const chargewire::client::Error& error) {
  SessionHealth health;
  try {
    ThrowChargewireErrors([&] { throw error; }, &health);
  } catch (const odbcabstraction::DriverException& e) {
    return {e, health.lost.load()};
  }
  ADD_FAILURE() << "no exception";
  return {odbcabstraction::DriverException("none"), false};
}

TEST(SessionLoss, IdleInTransactionIsATransactionRollback) {
  auto [e, lost] = Map(Problem(401, "session_ended", "idle_in_transaction"));
  EXPECT_EQ(e.GetSqlState(), "40000");
  EXPECT_NE(e.GetMessageText().find("idle_in_transaction"), std::string::npos) << e.GetMessageText();
  EXPECT_NE(e.GetMessageText().find("rolled back"), std::string::npos) << e.GetMessageText();
  EXPECT_NE(e.GetMessageText().find("detail from the server"), std::string::npos) << e.GetMessageText();
  EXPECT_TRUE(lost);
}

TEST(SessionLoss, SessionNotFoundIsALostLink) {
  auto [e, lost] = Map(Problem(404, "session_not_found"));
  EXPECT_EQ(e.GetSqlState(), "08S01");
  EXPECT_TRUE(lost);
}

TEST(SessionLoss, OtherReasonsNameTheReason) {
  auto [killed, killed_lost] = Map(Problem(401, "session_ended", "killed"));
  EXPECT_EQ(killed.GetSqlState(), "08S01");
  EXPECT_NE(killed.GetMessageText().find("reason killed"), std::string::npos) << killed.GetMessageText();
  EXPECT_TRUE(killed_lost);
  auto [revoked, revoked_lost] = Map(Problem(401, "session_ended", "token_revoked"));
  EXPECT_EQ(revoked.GetSqlState(), "28000");
  EXPECT_TRUE(revoked_lost);
}

TEST(SessionLoss, OtherErrorsLeaveTheSession) {
  auto [denied, denied_lost] = Map(Problem(403, "permission_denied"));
  EXPECT_EQ(denied.GetSqlState(), "HY000");
  EXPECT_FALSE(denied_lost);
  auto [token, token_lost] = Map(Problem(401, "invalid_token"));
  EXPECT_EQ(token.GetSqlState(), "28000");
  EXPECT_FALSE(token_lost);
}

}  // namespace
}  // namespace driver::chargewire_odbc
