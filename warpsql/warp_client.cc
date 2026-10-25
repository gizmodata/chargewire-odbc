// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
#include "warp_client.h"
#include <arrow/compute/api.h>

namespace driver::warpsql_odbc {
namespace {
std::shared_ptr<ResultInfo> Rows(warpsql::client::ExecuteResult result) {
  auto info = std::make_shared<ResultInfo>(result.reader ? result.reader->schema() : arrow::schema({}));
  info->native_reader = result.reader;
  if (result.reader) info->reader = result.reader->AsRecordBatchReader();
  if (result.document && result.document->rows_affected) info->rows = *result.document->rows_affected;
  return info;
}
std::unique_ptr<ResultInfo> Table(const std::shared_ptr<arrow::Table>& table) {
  auto info = std::make_unique<ResultInfo>(table->schema());
  info->reader = std::make_shared<arrow::TableBatchReader>(table);
  info->rows = table->num_rows();
  return info;
}
std::string OrEmpty(const std::string* s) { return s ? *s : ""; }
}
arrow::Result<std::shared_ptr<ResultInfo>> WarpPreparedStatement::Execute(const CallOptions& o) { return Rows(Run(o)); }
arrow::Result<int64_t> WarpPreparedStatement::ExecuteUpdate(const CallOptions& o) {
  auto result = Run(o);
  if (result.reader) ThrowWarpErrors([&] { result.reader->Close(); });
  return result.document && result.document->rows_affected ? *result.document->rows_affected : 0;
}
arrow::Result<int64_t> WarpSqlClient::ExecuteUpdate(const CallOptions& o, const std::string& sql) {
  auto r = ThrowWarpErrors([&] { return session_->Execute(sql, o.Native()); });
  if (r.reader) ThrowWarpErrors([&] { r.reader->Close(); });
  return r.document && r.document->rows_affected ? *r.document->rows_affected : 0;
}
arrow::Result<std::shared_ptr<ResultInfo>> WarpSqlClient::GetCatalogs(const CallOptions&) {
  return std::shared_ptr<ResultInfo>(Table(ThrowWarpErrors([&] { return session_->Catalogs(); })));
}
arrow::Result<std::shared_ptr<ResultInfo>> WarpSqlClient::GetDbSchemas(const CallOptions&, const std::string* cat, const std::string* schema) {
  return std::shared_ptr<ResultInfo>(Table(ThrowWarpErrors([&] { return session_->DbSchemas(OrEmpty(cat), OrEmpty(schema)); })));
}
arrow::Result<std::shared_ptr<ResultInfo>> WarpSqlClient::GetTables(const CallOptions&, const std::string* cat, const std::string* schema,
    const std::string* name, bool include_schema, const std::vector<std::string>* types) {
  warpsql::client::TablesRequest r;
  r.catalog = OrEmpty(cat); r.db_schema_filter_pattern = OrEmpty(schema); r.table_name_filter_pattern = OrEmpty(name);
  r.include_schema = include_schema; if (types) r.table_types = *types;
  return std::shared_ptr<ResultInfo>(Table(ThrowWarpErrors([&] { return session_->Tables(r); })));
}
arrow::Result<std::shared_ptr<ResultInfo>> WarpSqlClient::GetTableTypes(const CallOptions&) {
  return std::shared_ptr<ResultInfo>(Table(ThrowWarpErrors([&] { return session_->TableTypes(); })));
}
arrow::Result<std::shared_ptr<ResultInfo>> WarpSqlClient::GetXdbcTypeInfo(const CallOptions&) {
  return std::shared_ptr<ResultInfo>(Table(ThrowWarpErrors([&] { return session_->TypeInfo(); })));
}
arrow::Result<std::unique_ptr<ResultInfo>> WarpSqlClient::GetPrimaryKeys(const CallOptions&, const TableRef& t) {
  return Table(ThrowWarpErrors([&] { return session_->PrimaryKeys(t.catalog.value_or(""), t.db_schema.value_or(""), t.table); }));
}
arrow::Result<std::unique_ptr<ResultInfo>> WarpSqlClient::GetImportedKeys(const CallOptions&, const TableRef& t) {
  return Table(ThrowWarpErrors([&] { return session_->ImportedKeys(t.catalog.value_or(""), t.db_schema.value_or(""), t.table); }));
}
arrow::Result<std::unique_ptr<ResultInfo>> WarpSqlClient::GetExportedKeys(const CallOptions&, const TableRef& t) {
  return Table(ThrowWarpErrors([&] { return session_->ExportedKeys(t.catalog.value_or(""), t.db_schema.value_or(""), t.table); }));
}
arrow::Result<std::unique_ptr<ResultInfo>> WarpSqlClient::GetCrossReference(const CallOptions&, const TableRef& pk, const TableRef& fk) {
  auto table = ThrowWarpErrors([&] { return session_->ImportedKeys(fk.catalog.value_or(""), fk.db_schema.value_or(""), fk.table); });
  for (auto [name, val] : std::vector<std::pair<std::string, std::optional<std::string>>>{
      {"pk_catalog_name", pk.catalog}, {"pk_db_schema_name", pk.db_schema}, {"pk_table_name", pk.table}}) {
    if (!val) continue;
    ARROW_ASSIGN_OR_RAISE(auto mask, arrow::compute::CallFunction("equal", {table->GetColumnByName(name), arrow::Datum(*val)}));
    ARROW_ASSIGN_OR_RAISE(auto filtered, arrow::compute::Filter(table, mask));
    table = filtered.table();
  }
  return Table(table);
}
}
