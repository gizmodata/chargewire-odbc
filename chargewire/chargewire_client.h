// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
// Native Chargewire transport for the ODBC compatibility layer. No Flight/gRPC.
#pragma once
#include <chargewire/client.hpp>
#include <arrow/api.h>
#include <odbcabstraction/exceptions.h>
#include <atomic>
#include <chrono>
#include <functional>
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
// (SQL_ATTR_CONNECTION_DEAD) so pools discard it, and the application reconnects. So does
// a refused token the driver can't replace by itself (TokenRefusal).
struct SessionHealth {
  std::atomic<bool> lost{false};
  // Whether the client library renews this connection's token by itself (a password or
  // client-certificate sign-in), so an expired one is replaced on the next request.
  bool renewable = false;
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
// The SQLSTATE and message for a refused token (spec §4.2) that leaves the connection unusable,
// or nothing. After token_revoked, credentials_changed or principal_dropped the driver must not
// sign in again by itself, and token_expired is final for a token the library doesn't renew
// (an access token given in the connection string, a token exchange, a browser sign-in).
inline std::optional<std::pair<std::string, std::string>> TokenRefusal(const chargewire::client::Error& e, bool renewable) {
  if (e.code() != "invalid_token") return std::nullopt;
  const std::string& reason = e.reason();
  const bool lasting = reason == "token_revoked" || reason == "credentials_changed" || reason == "principal_dropped" ||
                     (reason == "token_expired" && !renewable);
  if (!lasting) return std::nullopt;
  return std::make_pair(std::string("28000"),
      "The server refused this connection's access token (invalid_token, reason " + reason + "). The request "
      "did not run; reconnect to continue (server: " + e.detail() + ")");
}
// Preserve server SQLSTATEs across the C ABI, including authentication, query errors,
// timeouts and cancellation. A transport failure is a communication-link failure.
template <class F> decltype(auto) ThrowChargewireErrors(F&& f, SessionHealth* health = nullptr) {
  try { return std::forward<F>(f)(); }
  catch (const chargewire::client::Error& e) {
    auto loss = SessionLoss(e);
    if (!loss) loss = TokenRefusal(e, health && health->renewable);
    if (loss) {
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
// Closing never signs in (Chargewire spec §4.1, §5.4). A close or a cancel that renewed the
// token would sign in to a server that may have been stopped since: a stopped server behind a
// resume holder starts again on a sign-in (which costs its owner money, and undoes a person's
// Stop) only to be told the session is gone. The client library renews a password or
// certificate sign-in only on requests that do work; its closes and cancels carry the token it
// has and give up within seconds on a server that is resuming. On top of that:
// - a freed prepared statement sends nothing; its DELETE goes ahead of the connection's next
//   request that does work, or the session's end frees it (so a disconnect sends one DELETE);
// - from BeginClose (SQLDisconnect) on, the library holds no saved sign-in at all, so nothing
//   sent while the connection closes can sign in.
class ChargewireClient {
  std::unique_ptr<chargewire::client::Client> client_;
  std::shared_ptr<SessionHealth> health_ = std::make_shared<SessionHealth>();
  std::atomic<bool> closing_ = false;
  // Lazy connect to a stopped server (Chargewire spec §11.2): nothing signed in or created yet;
  // the first request that does work does both.
  std::atomic<bool> waiting_ = false;
  // Lazy connect: what the connection would have done on its session as it connected (a
  // product's session settings), done instead just before its first request that does work.
  std::function<void()> first_work_;
  std::atomic<bool> first_work_pending_ = false;
  std::mutex first_work_mutex_;
  void RunFirstWork() {
    if (!first_work_pending_) return;
    std::lock_guard lock(first_work_mutex_);  // other threads' first requests wait for it
    if (!first_work_pending_) return;
    auto first = std::move(first_work_);
    first_work_pending_ = false;  // its own requests are work too, and must not run it again
    try {
      first();
    } catch (...) {
      // Not done: the next request tries again, so none runs without it.
      first_work_ = std::move(first);
      first_work_pending_ = true;
      throw;
    }
  }
  // Declared before session_ so they are destroyed after it: once the session is closed, the
  // library sends nothing for them.
  std::shared_ptr<DeferredCloses> deferred_ = std::make_shared<DeferredCloses>();
  std::unique_ptr<chargewire::client::Session> session_;
  template <class F> decltype(auto) Call(F&& f) { return ThrowChargewireErrors(std::forward<F>(f), health_.get()); }
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
  // A request that does work (the library may renew the token for it); freed statements'
  // closes go first.
  template <class F> decltype(auto) Work(F&& f) {
    CloseDeferred();
    RunFirstWork();
    struct Started {  // once the session exists, the connection is no longer waiting
      ChargewireClient& c;
      ~Started() { if (c.waiting_ && c.session_ && !c.session_->id().empty()) c.waiting_ = false; }
    } started{*this};
    return Call(std::forward<F>(f));
  }
  // `renewable`: the library renews this sign-in's token by itself (SessionHealth::renewable).
  explicit ChargewireClient(std::unique_ptr<chargewire::client::Client> client, const boost::json::object& options,
                            bool renewable = false)
    : client_(std::move(client)),
      session_(Call([&] { health_->renewable = renewable; return client_->CreateSession(options); })) {
    waiting_ = session_->id().empty();  // the client library held the sign-in and session back
  }
  // Whether the connection connected lazily to a stopped server and has sent nothing since
  // (no sign-in, no session): what it reports then comes from the driver, not the server.
  bool waiting_for_work() const { return waiting_; }
  // Lazy connect: `first` runs just before the connection's first request that does work, on
  // the session that request opens; if it fails, that request fails with its error.
  void BeforeFirstWork(std::function<void()> first) {
    std::lock_guard lock(first_work_mutex_);
    first_work_ = std::move(first);
    first_work_pending_ = static_cast<bool>(first_work_);
  }
  // The connection is closing: from now on no request renews the token or signs in.
  void BeginClose() {
    if (closing_.exchange(true)) return;
    // The same token, read without renewing it, with the saved sign-in dropped.
    client_->SetAccessToken(client_->current_access_token());
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
  // Sends a cancel only while a statement runs, never a sign-in, and gives up within seconds.
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
