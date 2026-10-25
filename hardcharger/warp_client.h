// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
// Native Warp SQL transport for the ODBC compatibility layer. No Flight/gRPC.
#pragma once
#include <warpsql/client.hpp>
#include <arrow/api.h>
#include <odbcabstraction/exceptions.h>
#include <chrono>
#include <utility>

namespace driver::hardcharger {
using TimeoutDuration = std::chrono::duration<double>;
struct CallOptions {
  TimeoutDuration timeout{-1};
  std::vector<std::pair<std::string, std::string>> headers;
  std::string query_tag;
  warpsql::client::StatementOptions Native() const {
    warpsql::client::StatementOptions o;
    if (timeout.count() > 0) o.timeout_ms = static_cast<int64_t>(timeout.count() * 1000);
    o.query_tag = query_tag;
    return o;
  }
};
// Preserve server SQLSTATEs across the C ABI, including authentication, query errors,
// timeouts and cancellation. A transport failure is a communication-link failure.
template <class F> decltype(auto) ThrowWarpErrors(F&& f) {
  try { return std::forward<F>(f)(); }
  catch (const warpsql::client::Error& e) {
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
  std::shared_ptr<warpsql::client::ResultReader> native_reader;
  int64_t rows = -1;
  explicit ResultInfo(std::shared_ptr<arrow::Schema> schema) : schema(std::move(schema)) {}
  arrow::Result<std::shared_ptr<arrow::Schema>> GetSchema(void*) const { return schema; }
  int64_t total_records() const { return rows; }
  void Close() { if (native_reader) ThrowWarpErrors([&] { native_reader->Close(); }); reader.reset(); native_reader.reset(); }
};
class WarpPreparedStatement {
  std::unique_ptr<warpsql::client::PreparedStatement> stmt_;
  std::shared_ptr<arrow::RecordBatch> params_;
public:
  explicit WarpPreparedStatement(std::unique_ptr<warpsql::client::PreparedStatement> stmt) : stmt_(std::move(stmt)) {}
  auto parameter_schema() const { return stmt_->parameter_schema(); }
  auto dataset_schema() const {
    return stmt_->result_kind() == "update" || !stmt_->result_schema() ? arrow::schema({}) : stmt_->result_schema();
  }
  arrow::Status SetParameters(const std::shared_ptr<arrow::RecordBatch>& params) { params_ = params; return arrow::Status::OK(); }
  warpsql::client::ExecuteResult Run(const CallOptions& options) {
    return ThrowWarpErrors([&] {
      if (!params_) return stmt_->Execute(options.Native());
      auto reader = arrow::RecordBatchReader::Make({params_}, params_->schema()).ValueOrDie();
      return stmt_->Execute(*reader, options.Native());
    });
  }
  arrow::Result<std::shared_ptr<ResultInfo>> Execute(const CallOptions& options);
  arrow::Result<int64_t> ExecuteUpdate(const CallOptions& options);
  arrow::Status Close(const CallOptions&) { ThrowWarpErrors([&] { stmt_->Close(); }); return arrow::Status::OK(); }
};
class WarpSqlClient {
  std::unique_ptr<warpsql::client::Client> client_;
  std::unique_ptr<warpsql::client::Session> session_;
public:
  explicit WarpSqlClient(std::unique_ptr<warpsql::client::Client> client, const boost::json::object& options)
    : client_(std::move(client)), session_(ThrowWarpErrors([&] { return client_->CreateSession(options); })) {}
  arrow::Result<std::shared_ptr<WarpPreparedStatement>> Prepare(const CallOptions&, const std::string& sql, const Transaction&) {
    return ThrowWarpErrors([&] { return std::make_shared<WarpPreparedStatement>(session_->Prepare(sql)); });
  }
  arrow::Result<int64_t> ExecuteUpdate(const CallOptions& options, const std::string& sql);
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
    ThrowWarpErrors([&] { session_->SetOptions({{"autocommit", false}}); }); return Transaction("active");
  }
  arrow::Status Commit(const CallOptions&, const Transaction&) {
    ThrowWarpErrors([&] { session_->Commit(); session_->SetOptions({{"autocommit", true}}); }); return arrow::Status::OK();
  }
  arrow::Status Rollback(const CallOptions&, const Transaction&) {
    ThrowWarpErrors([&] { session_->Rollback(); session_->SetOptions({{"autocommit", true}}); }); return arrow::Status::OK();
  }
  auto ServerInfo() { return ThrowWarpErrors([&] { return session_->ServerInfo(); }); }
  void Cancel() { ThrowWarpErrors([&] { session_->CancelRunning(); }); }
  void Close() { ThrowWarpErrors([&] { session_->Close(); }); }
};
}
