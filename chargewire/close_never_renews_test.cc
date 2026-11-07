// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
// Closing never signs in. The client library renews a password sign-in shortly before its
// token expires, on whatever request comes next; a disconnect, a freed statement or a cancel
// that did so would sign in to a server that may have been stopped since, and a stopped server
// behind a resume holder starts again on a sign-in. A stub server here hands out tokens that
// pass the library's renew point (60 s before expiry) a moment after they are issued, and the
// tests count the sign-ins.

#include "chargewire_connection.h"

#include <odbcabstraction/exceptions.h>
#include <odbcabstraction/platform.h>
#include <odbcabstraction/spi/statement.h>

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/writer.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "test_stub_server.h"

namespace driver::chargewire_odbc {
namespace {

using namespace std::chrono_literals;
using namespace test_stub;
using odbcabstraction::Statement;

// Tokens last 61 s, so after kPastRenewPoint the connection's token is one any request renews.
constexpr int kLifetime = 61;
constexpr auto kPastRenewPoint = 1500ms;

const std::string kSession = std::string(kSessions) + "/s1";
const std::string kPrepared = kSession + "/prepared";

Answer Problem(int status, const std::string& code, const std::string& reason = "") {
  std::string body = R"({"status":)" + std::to_string(status) + R"(,"code":")" + code + R"(","detail":"from the stub")";
  if (!reason.empty()) body += R"(,"reason":")" + reason + R"(")";
  return {status, "application/problem+json", body + "}", {}};
}

// Serves sign-ins, session s1 and prepared statements p0, p1, ...
Answer Server(const std::string& method, const std::string& path, int nth) {
  if (method == "POST" && path == kPrepared) {
    return {201, "application/json", R"({"prepared_id":"p)" + std::to_string(nth) + R"(","result_kind":"update"})", {}};
  }
  return Serving(method, path, kLifetime);
}

struct Connected {
  StubServer& stub;
  ChargewireConnection connection{odbcabstraction::V_3};
  explicit Connected(StubServer& s, const std::map<std::string, std::string>& extra = {}) : stub(s) {
    std::vector<std::string> missing;
    connection.Connect(stub.Properties(extra), missing);
  }
  int SignIns() { return stub.Count("POST", kToken); }
};

std::shared_ptr<Statement> Prepared(ChargewireConnection& connection) {
  auto statement = connection.CreateStatement();
  statement->Prepare("SELECT 1");
  return statement;
}

TEST(CloseNeverRenews, ADisconnectAfterTheRenewPointSendsNoSignIn) {
  StubServer stub(Server);
  Connected c(stub);
  auto statement = Prepared(c.connection);
  std::this_thread::sleep_for(kPastRenewPoint);
  const int before = c.SignIns();
  // The order SQLDisconnect closes in, without the ODBC layer's BeginClose (a wrapper's).
  statement.reset();
  c.connection.Close();
  EXPECT_EQ(c.SignIns(), before);
  EXPECT_EQ(stub.Count("DELETE", kSession), 1);
  // The session's end frees its prepared statements; nothing is sent for them.
  EXPECT_EQ(stub.Count("DELETE", kPrepared + "/p0"), 0);
}

TEST(CloseNeverRenews, RequestsThatDoWorkStillRenew) {
  StubServer stub(Server);
  Connected c(stub);
  std::this_thread::sleep_for(kPastRenewPoint);
  const int before = c.SignIns();
  auto statement = Prepared(c.connection);
  EXPECT_EQ(c.SignIns(), before + 1);
  statement.reset();
  c.connection.Close();
  EXPECT_EQ(c.SignIns(), before + 1);
}

TEST(CloseNeverRenews, AFreedStatementSendsNothingAndItsCloseGoesWithTheNextWork) {
  StubServer stub(Server);
  Connected c(stub);
  auto first = Prepared(c.connection);
  std::this_thread::sleep_for(kPastRenewPoint);
  const auto seen = stub.Seen().size();
  first.reset();  // SQLFreeHandle(SQL_HANDLE_STMT)
  EXPECT_EQ(stub.Seen().size(), seen) << "freeing a statement sent a request";
  const int before = c.SignIns();
  auto second = Prepared(c.connection);  // work: renews once, and the freed statement's close goes first
  EXPECT_EQ(c.SignIns(), before + 1);
  const auto all = stub.Seen();
  ASSERT_GE(all.size(), 2u);
  EXPECT_EQ(all[all.size() - 2], "DELETE " + kPrepared + "/p0");
  EXPECT_EQ(all.back(), "POST " + kPrepared);
  second.reset();
  c.connection.Close();
  EXPECT_EQ(c.SignIns(), before + 1);
}

TEST(CloseNeverRenews, CancellingSendsNoSignIn) {
  StubServer stub(Server);
  Connected c(stub);
  auto statement = Prepared(c.connection);
  std::this_thread::sleep_for(kPastRenewPoint);
  const int before = c.SignIns();
  const auto seen = stub.Seen().size();
  statement->Cancel();  // SQLCancel with nothing running sends nothing at all
  EXPECT_EQ(c.SignIns(), before);
  EXPECT_EQ(stub.Seen().size(), seen);
  c.connection.BeginClose();
  statement.reset();
  c.connection.Close();
  EXPECT_EQ(c.SignIns(), before);
}

TEST(CloseNeverRenews, ADisconnectRefusedWithInvalidTokenSucceedsWithoutASignIn) {
  StubServer stub([](const std::string& method, const std::string& path, int nth) {
    if (method == "DELETE") return Problem(401, "invalid_token", "token_revoked");
    return Server(method, path, nth);
  });
  Connected c(stub);
  auto statement = Prepared(c.connection);
  std::this_thread::sleep_for(kPastRenewPoint);
  const int before = c.SignIns();
  c.connection.BeginClose();
  statement.reset();
  EXPECT_NO_THROW(c.connection.Close());
  EXPECT_EQ(c.SignIns(), before);
  EXPECT_EQ(stub.Count("DELETE", kSession), 1);
}

// A disconnect from a server that is resuming takes seconds, not ResumeTimeout.
TEST(CloseNeverRenews, ADisconnectFromAResumingServerIsBounded) {
  std::atomic<bool> stopped = false;
  StubServer stub([&](const std::string& method, const std::string& path, int nth) {
    if (stopped) return Resuming("1");
    return Server(method, path, nth);
  });
  Connected c(stub, {{"ResumeTimeout", "30"}});
  auto statement = Prepared(c.connection);
  std::this_thread::sleep_for(kPastRenewPoint);
  const int before = c.SignIns();
  stopped = true;
  const auto started = std::chrono::steady_clock::now();
  c.connection.BeginClose();
  statement.reset();
  EXPECT_NO_THROW(c.connection.Close());
  EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
  EXPECT_EQ(c.SignIns(), before);
}

// An Arrow IPC stream of `batches` batches of 1000 BIGINTs.
std::string Stream(int batches) {
  arrow::Int64Builder builder;
  for (int i = 0; i < 1000; i++) EXPECT_TRUE(builder.Append(i).ok());
  auto column = builder.Finish().ValueOrDie();
  auto schema = arrow::schema({arrow::field("n", arrow::int64())});
  auto batch = arrow::RecordBatch::Make(schema, column->length(), {column});
  auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
  auto writer = arrow::ipc::MakeStreamWriter(sink, schema).ValueOrDie();
  for (int i = 0; i < batches; i++) EXPECT_TRUE(writer->WriteRecordBatch(*batch).ok());
  EXPECT_TRUE(writer->Close().ok());
  return sink->Finish().ValueOrDie()->ToString();
}

// SQLDisconnect with a split result still open (spec §6.10): closing it cancels the statement,
// and that cancel must not sign in either.
TEST(CloseNeverRenews, ADisconnectWithAnOpenSplitResultSendsNoSignIn) {
  const std::string rows = Stream(200);
  const std::string statement_path = kSession + "/statements/st1";
  StubServer stub([&](const std::string& method, const std::string& path, int nth) -> Answer {
    if (method == "GET" && path == "/chargewire") {
      return {200, "application/json",
              R"({"protocol":"chargewire","protocol_versions":["1"],"server":{"name":"stub","version":"0"},)"
              R"("capabilities":["result_partitions"],"auth":{"grant_types":["password"]}})",
              {}};
    }
    if (method == "POST" && path == kSession + "/statements") {
      return {200, "application/vnd.apache.arrow.stream", rows,
              {{"Chargewire-Statement-Id", "st1"}, {"Chargewire-Result-Partitions", "2"},
               {"Chargewire-Result-Order", "interleaved"}}};
    }
    if (method == "GET" && path == statement_path + "/result") return {200, "application/vnd.apache.arrow.stream", rows, {}};
    if (method == "POST" && path == statement_path + "/cancel") return {202, "application/json", "{}", {}};
    return Server(method, path, nth);
  });
  Connected c(stub);
  auto statement = c.connection.CreateStatement();
  ASSERT_TRUE(statement->Execute("SELECT n FROM big"));
  std::this_thread::sleep_for(kPastRenewPoint);
  const int before = c.SignIns();
  // As ODBCConnection::disconnect does: BeginClose, release the statements, Close.
  c.connection.BeginClose();
  statement.reset();
  c.connection.Close();
  EXPECT_EQ(stub.Count("POST", statement_path + "/cancel"), 1);
  EXPECT_EQ(c.SignIns(), before);
}

}  // namespace
}  // namespace driver::chargewire_odbc
