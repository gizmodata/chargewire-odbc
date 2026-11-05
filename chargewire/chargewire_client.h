// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
// Native Chargewire transport for the ODBC compatibility layer. No Flight/gRPC.
#pragma once
#include <chargewire/client.hpp>
#include <arrow/api.h>
#include <odbcabstraction/exceptions.h>
#include <atomic>
#include <chrono>
#include <optional>
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
class ChargewirePreparedStatement {
  std::unique_ptr<chargewire::client::PreparedStatement> stmt_;
  std::shared_ptr<arrow::RecordBatch> params_;
  std::shared_ptr<SessionHealth> health_;
public:
  ChargewirePreparedStatement(std::unique_ptr<chargewire::client::PreparedStatement> stmt, std::shared_ptr<SessionHealth> health)
    : stmt_(std::move(stmt)), health_(std::move(health)) {}
  const std::shared_ptr<SessionHealth>& health() const { return health_; }
  auto parameter_schema() const { return stmt_->parameter_schema(); }
  auto dataset_schema() const {
    return stmt_->result_kind() == "update" || !stmt_->result_schema() ? arrow::schema({}) : stmt_->result_schema();
  }
  arrow::Status SetParameters(const std::shared_ptr<arrow::RecordBatch>& params) { params_ = params; return arrow::Status::OK(); }
  chargewire::client::ExecuteResult Run(const CallOptions& options) {
    return ThrowChargewireErrors([&] {
      if (!params_) return stmt_->Execute(options.Native());
      auto reader = arrow::RecordBatchReader::Make({params_}, params_->schema()).ValueOrDie();
      return stmt_->Execute(*reader, options.Native());
    }, health_.get());
  }
  arrow::Result<std::shared_ptr<ResultInfo>> Execute(const CallOptions& options);
  arrow::Result<int64_t> ExecuteUpdate(const CallOptions& options);
  arrow::Status Close(const CallOptions&) { ThrowChargewireErrors([&] { stmt_->Close(); }, health_.get()); return arrow::Status::OK(); }
};
class ChargewireClient {
  std::unique_ptr<chargewire::client::Client> client_;
  std::shared_ptr<SessionHealth> health_ = std::make_shared<SessionHealth>();
  std::unique_ptr<chargewire::client::Session> session_;
  template <class F> decltype(auto) Call(F&& f) { return ThrowChargewireErrors(std::forward<F>(f), health_.get()); }
public:
  explicit ChargewireClient(std::unique_ptr<chargewire::client::Client> client, const boost::json::object& options)
    : client_(std::move(client)), session_(Call([&] { return client_->CreateSession(options); })) {}
  arrow::Result<std::shared_ptr<ChargewirePreparedStatement>> Prepare(const CallOptions&, const std::string& sql, const Transaction&) {
    return Call([&] { return std::make_shared<ChargewirePreparedStatement>(session_->Prepare(sql), health_); });
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
    Call([&] { session_->SetOptions({{"autocommit", false}}); }); return Transaction("active");
  }
  arrow::Status Commit(const CallOptions&, const Transaction&) {
    Call([&] { session_->Commit(); session_->SetOptions({{"autocommit", true}}); }); return arrow::Status::OK();
  }
  arrow::Status Rollback(const CallOptions&, const Transaction&) {
    // The server rolled back a lost session's transaction already: nothing is left to undo.
    if (session_lost()) return arrow::Status::OK();
    Call([&] { session_->Rollback(); session_->SetOptions({{"autocommit", true}}); }); return arrow::Status::OK();
  }
  auto ServerInfo() { return Call([&] {
    auto info = session_->ServerInfo();
    const auto discovery = client_->Discover();
    info["server_name"] = discovery.server_name;
    info["server_version"] = discovery.server_version;
    return info;
  }); }
  void Cancel() { Call([&] { session_->CancelRunning(); }); }
  // Closing a session the server no longer has has nothing to close.
  void Close() { if (!session_lost()) Call([&] { session_->Close(); }); }
  bool session_lost() const { return health_->lost; }
  const std::shared_ptr<SessionHealth>& health() const { return health_; }
};
}
