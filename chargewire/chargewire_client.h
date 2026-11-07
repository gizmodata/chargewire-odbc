// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
// Native Chargewire transport for the ODBC compatibility layer. No Flight/gRPC.
#pragma once
#include <chargewire/client.hpp>
#include <arrow/api.h>
#include <odbcabstraction/exceptions.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace driver::chargewire_odbc {
using TimeoutDuration = std::chrono::duration<double>;
struct CallOptions {
  TimeoutDuration timeout{-1};
  std::vector<std::pair<std::string, std::string>> headers;
  std::string query_tag;
  chargewire::client::StatementOptions Native() const {
    chargewire::client::StatementOptions o;
    if (timeout.count() > 0) o.timeout_ms = static_cast<int64_t>(timeout.count() * 1000);
    o.query_tag = query_tag;
    return o;
  }
};
// Whether the server no longer has this connection's session (spec §5.6): it answered
// session_not_found or session_ended. The driver never replaces a lost session, and so
// never runs a statement again on a new one: a transaction begun in SQL (BEGIN) is
// invisible to it, and re-running a statement in autocommit after the server rolled that
// transaction back would commit it on its own. The connection reports itself dead
// (SQL_ATTR_CONNECTION_DEAD) so pools discard it, and the application reconnects.
struct SessionHealth {
  std::atomic<bool> lost{false};
};
// The SQLSTATE and message for an error that says the session is gone, or nothing.
inline std::optional<std::pair<std::string, std::string>> SessionLoss(const chargewire::client::Error& e) {
  const std::string server = " (server: " + e.detail() + ")";
  if (e.code() == "session_not_found") {
    return std::make_pair(std::string("08S01"),
        "The server no longer has this connection's session (it was closed, evicted after idling, or the "
        "server restarted), and any transaction open in it was rolled back. The request did not run; "
        "reconnect to continue" + server);
  }
  if (e.code() != "session_ended") return std::nullopt;
  const std::string reason = e.reason().empty() ? "unknown" : e.reason();
  if (reason == "idle_in_transaction") {
    // The transaction was rolled back: class 40 (transaction rollback).
    return std::make_pair(std::string("40000"),
        "The server ended this connection's session (session_ended, reason idle_in_transaction): its "
        "transaction sat idle longer than the server's idle-in-transaction timeout and was rolled back. "
        "The request did not run; reconnect and run the transaction again" + server);
  }
  // credentials_changed, principal_dropped, token_revoked, token_expired: an authorization
  // problem (28000); killed, or a reason this driver doesn't know: the link is gone (08S01).
  const bool auth = reason == "credentials_changed" || reason == "principal_dropped" ||
                    reason == "token_revoked" || reason == "token_expired";
  return std::make_pair(std::string(e.sqlstate().empty() ? (auth ? "28000" : "08S01") : e.sqlstate()),
      "The server ended this connection's session (session_ended, reason " + reason + "), and any "
      "transaction open in it was rolled back. The request did not run; reconnect to continue" + server);
}
// Preserve server SQLSTATEs across the C ABI, including authentication, query errors,
// timeouts and cancellation. A transport failure is a communication-link failure.
template <class F> decltype(auto) ThrowChargewireErrors(F&& f, SessionHealth* health = nullptr) {
  try { return std::forward<F>(f)(); }
  catch (const chargewire::client::Error& e) {
    if (auto loss = SessionLoss(e)) {
      if (health) health->lost = true;
      throw odbcabstraction::DriverException(loss->second, loss->first);
    }
    std::string state = e.sqlstate();
    if (state.empty()) {
      if (e.IsTransport()) state = "08S01";
      else if (e.status() == 401) state = "28000";
      else if (e.code() == "statement_timeout") state = "HYT00";
      // Still resuming when ResumeTimeout ran out (or with the retry off): the server is on
      // its way but did not serve in time.
      else if (e.code() == "server_resuming") state = "HYT01";
      else if (e.IsCancelled()) state = "HY008";
      else state = "HY000";
    }
    throw odbcabstraction::DriverException(e.detail(), state);
  }
}
struct Transaction {
  std::string id;
  explicit Transaction(std::string id = "") : id(std::move(id)) {}
  bool is_valid() const { return !id.empty(); }
};
struct TableRef { std::optional<std::string> catalog, db_schema; std::string table; };
struct StreamChunk { std::shared_ptr<arrow::RecordBatch> data; };
struct ResultInfo {
  std::shared_ptr<arrow::Schema> schema;
  std::shared_ptr<arrow::RecordBatchReader> reader;
  std::shared_ptr<chargewire::client::ResultReader> native_reader;
  int64_t rows = -1;
  std::shared_ptr<SessionHealth> health;
  explicit ResultInfo(std::shared_ptr<arrow::Schema> schema) : schema(std::move(schema)) {}
  arrow::Result<std::shared_ptr<arrow::Schema>> GetSchema(void*) const { return schema; }
  int64_t total_records() const { return rows; }
  void Close() { if (native_reader) ThrowChargewireErrors([&] { native_reader->Close(); }, health.get()); reader.reset(); native_reader.reset(); }
};
class ChargewireClient;
// Prepared statements the application freed, not closed yet (ChargewireClient).
struct DeferredCloses {
  std::mutex mutex;
  std::vector<std::unique_ptr<chargewire::client::PreparedStatement>> statements;
};
class ChargewirePreparedStatement {
  std::unique_ptr<chargewire::client::PreparedStatement> stmt_;
  std::shared_ptr<arrow::RecordBatch> params_;
  std::shared_ptr<SessionHealth> health_;
  ChargewireClient* owner_ = nullptr;  // the connection's client, which outlives its statements
  std::weak_ptr<DeferredCloses> closes_;
public:
  ChargewirePreparedStatement(std::unique_ptr<chargewire::client::PreparedStatement> stmt, std::shared_ptr<SessionHealth> health,
                              ChargewireClient* owner = nullptr, std::weak_ptr<DeferredCloses> closes = {})
    : stmt_(std::move(stmt)), health_(std::move(health)), owner_(owner), closes_(std::move(closes)) {}
  const std::shared_ptr<SessionHealth>& health() const { return health_; }
  auto parameter_schema() const { return stmt_->parameter_schema(); }
  auto dataset_schema() const {
    return stmt_->result_kind() == "update" || !stmt_->result_schema() ? arrow::schema({}) : stmt_->result_schema();
  }
  arrow::Status SetParameters(const std::shared_ptr<arrow::RecordBatch>& params) { params_ = params; return arrow::Status::OK(); }
  chargewire::client::ExecuteResult Run(const CallOptions& options);
  arrow::Result<std::shared_ptr<ResultInfo>> Execute(const CallOptions& options);
  arrow::Result<int64_t> ExecuteUpdate(const CallOptions& options);
  // Sends nothing: the connection's client closes it with its next request that does work,
  // or the session's end frees it (ChargewireClient).
  arrow::Status Close(const CallOptions&);
};
// Closing never signs in (Chargewire spec §4.1, §5.4). The client library renews a password
// or certificate sign-in shortly before its token expires, on whatever request comes next. A
// close or a cancel that renewed would sign in to a server that may have been stopped since:
// a stopped server behind a resume holder starts again on a sign-in (which costs its owner
// money, and undoes a person's Stop) only to be told the session is gone. So:
// - a freed prepared statement sends nothing; its DELETE goes ahead of the connection's next
//   request that does work (which renews anyway), or the session's end frees it;
// - from BeginClose (SQLDisconnect) on, every request carries the token the connection last
//   used, never renewed: a refused or expired one means the session is gone anyway, and the
//   session's DELETE (bounded to 3 s by the client library) ignores errors.
// Still renewed by the library, and needing a change there: SQLCancel while a statement runs,
// and SQLCloseCursor on a split result not read to its end (its cancel), on a live connection.
class ChargewireClient {
  std::unique_ptr<chargewire::client::Client> client_;
  std::shared_ptr<SessionHealth> health_ = std::make_shared<SessionHealth>();
  std::mutex mutex_;
  std::string token_;    // the token the last request that did work carried
  bool closing_ = false;
  // Declared before session_ so they are destroyed after it: once the session is closed, the
  // library sends nothing for them.
  std::shared_ptr<DeferredCloses> deferred_ = std::make_shared<DeferredCloses>();
  std::unique_ptr<chargewire::client::Session> session_;
  template <class F> decltype(auto) Call(F&& f) { return ThrowChargewireErrors(std::forward<F>(f), health_.get()); }
  // The token this request will carry, renewed first when it is about to expire, as the
  // request itself would renew it.
  void KeepToken() {
    auto token = Call([&] { return client_->access_token(); });
    std::lock_guard lock(mutex_);
    if (!closing_) token_ = std::move(token);
  }
  // Best effort: a failure here leaves the statement to the session's end.
  void CloseDeferred() {
    std::vector<std::unique_ptr<chargewire::client::PreparedStatement>> closes;
    {
      std::lock_guard lock(deferred_->mutex);
      closes.swap(deferred_->statements);
    }
    for (auto& stmt : closes) {
      try { stmt->Close(); } catch (...) {}
    }
  }
public:
  // A request that does work: it may renew the token, and freed statements' closes go first.
  template <class F> decltype(auto) Work(F&& f) {
    KeepToken();
    CloseDeferred();
    return Call(std::forward<F>(f));
  }
  explicit ChargewireClient(std::unique_ptr<chargewire::client::Client> client, const boost::json::object& options)
    : client_(std::move(client)), session_(Work([&] { return client_->CreateSession(options); })) {}
  // The connection is closing: from now on no request renews the token or signs in.
  void BeginClose() {
    std::string token;
    {
      std::lock_guard lock(mutex_);
      if (closing_) return;
      closing_ = true;
      token = token_;
    }
    client_->SetAccessToken(token);  // drops the saved sign-in, so nothing renews it
  }
  arrow::Result<std::shared_ptr<ChargewirePreparedStatement>> Prepare(const CallOptions&, const std::string& sql, const Transaction&) {
    return Work([&] { return std::make_shared<ChargewirePreparedStatement>(session_->Prepare(sql), health_, this, deferred_); });
  }
  arrow::Result<int64_t> ExecuteUpdate(const CallOptions& options, const std::string& sql);
  // Runs SQL directly on the statements endpoint (spec §6.1), without a prepare: the
  // server takes statements here that it refuses to prepare (SET hardcharger.*, KILL SESSION).
  arrow::Result<std::shared_ptr<ResultInfo>> Execute(const CallOptions& options, const std::string& sql);
  arrow::Result<std::shared_ptr<ResultInfo>> GetCatalogs(const CallOptions&);
  arrow::Result<std::shared_ptr<ResultInfo>> GetDbSchemas(const CallOptions&, const std::string*, const std::string*);
  arrow::Result<std::shared_ptr<ResultInfo>> GetTables(const CallOptions&, const std::string*, const std::string*, const std::string*, bool, const std::vector<std::string>*);
  arrow::Result<std::shared_ptr<ResultInfo>> GetTableTypes(const CallOptions&);
  arrow::Result<std::shared_ptr<ResultInfo>> GetXdbcTypeInfo(const CallOptions&);
  arrow::Result<std::unique_ptr<ResultInfo>> GetPrimaryKeys(const CallOptions&, const TableRef&);
  arrow::Result<std::unique_ptr<ResultInfo>> GetImportedKeys(const CallOptions&, const TableRef&);
  arrow::Result<std::unique_ptr<ResultInfo>> GetExportedKeys(const CallOptions&, const TableRef&);
  arrow::Result<std::unique_ptr<ResultInfo>> GetCrossReference(const CallOptions&, const TableRef&, const TableRef&);
  arrow::Result<Transaction> BeginTransaction(const CallOptions&) {
    Work([&] { session_->SetOptions({{"autocommit", false}}); }); return Transaction("active");
  }
  arrow::Status Commit(const CallOptions&, const Transaction&) {
    Work([&] { session_->Commit(); session_->SetOptions({{"autocommit", true}}); }); return arrow::Status::OK();
  }
  arrow::Status Rollback(const CallOptions&, const Transaction&) {
    // The server rolled back a lost session's transaction already: nothing is left to undo.
    if (session_lost()) return arrow::Status::OK();
    Work([&] { session_->Rollback(); session_->SetOptions({{"autocommit", true}}); }); return arrow::Status::OK();
  }
  auto ServerInfo() { return Work([&] {
    auto info = session_->ServerInfo();
    const auto discovery = client_->Discover();
    info["server_name"] = discovery.server_name;
    info["server_version"] = discovery.server_version;
    return info;
  }); }
  // Sends a cancel only while a statement runs; the client library may renew the token for it.
  void Cancel() { Call([&] { session_->CancelRunning(); }); }
  // Closing a session the server no longer has has nothing to close. The library's close
  // ignores errors (a refused token, a server gone) and waits at most 3 s.
  void Close() {
    BeginClose();
    if (!session_lost()) Call([&] { session_->Close(); });
  }
  bool session_lost() const { return health_->lost; }
  const std::shared_ptr<SessionHealth>& health() const { return health_; }
};
inline chargewire::client::ExecuteResult ChargewirePreparedStatement::Run(const CallOptions& options) {
  if (!stmt_) throw odbcabstraction::DriverException("the prepared statement is closed", "HY010");
  auto run = [&] {
    if (!params_) return stmt_->Execute(options.Native());
    auto reader = arrow::RecordBatchReader::Make({params_}, params_->schema()).ValueOrDie();
    return stmt_->Execute(*reader, options.Native());
  };
  if (owner_) return owner_->Work(run);
  return ThrowChargewireErrors(run, health_.get());
}
inline arrow::Status ChargewirePreparedStatement::Close(const CallOptions&) {
  if (!stmt_) return arrow::Status::OK();
  if (auto closes = closes_.lock()) {
    std::lock_guard lock(closes->mutex);
    closes->statements.push_back(std::move(stmt_));
  } else {
    ThrowChargewireErrors([&] { stmt_->Close(); }, health_.get());  // no client to hand it to
    stmt_.reset();
  }
  return arrow::Status::OK();
}
}
