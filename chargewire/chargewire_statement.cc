/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "chargewire_statement.h"
#include "chargewire_connection.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <ctime>
#include <odbcabstraction/platform.h>
#include "chargewire_result_set.h"
#include "chargewire_result_set_metadata.h"
#include "parameter_batch.h"
#include "chargewire_statement_get_columns.h"
#include "chargewire_statement_get_tables.h"
#include "chargewire_statement_get_type_info.h"
#include "record_batch_transformer.h"
#include "odbc_escape_sequences.h"
#include "utils.h"
#include <arrow/io/memory.h>
#include <arrow/builder.h>
#include <arrow/record_batch.h>
#include <arrow/type.h>
#include <sql.h>
#include <sqlext.h>

#include <boost/optional.hpp>
#include <utility>
#include <odbcabstraction/exceptions.h>
#include <odbcabstraction/encoding.h>

namespace driver {
namespace chargewire_odbc {

using arrow::Result;
using arrow::Status;
using driver::odbcabstraction::DriverException;
using driver::odbcabstraction::ResultSet;
using driver::odbcabstraction::ResultSetMetadata;
using driver::odbcabstraction::Statement;

namespace {

void ClosePreparedStatementIfAny(
    std::shared_ptr<ChargewirePreparedStatement>
        &prepared_statement,
    const CallOptions &call_options) {
  if (prepared_statement != nullptr) {
    ThrowIfNotOK(prepared_statement->Close(call_options));
    prepared_statement.reset();
  }
}

} // namespace

ChargewireStatement::ChargewireStatement(
    const odbcabstraction::Diagnostics& diagnostics,
    ChargewireClient &sql_client,
    CallOptions call_options,
    const odbcabstraction::MetadataSettings& metadata_settings,
    ChargewireConnection& connection)
    : diagnostics_("GizmoData", diagnostics.GetDataSourceComponent(), diagnostics.GetOdbcVersion()),
      sql_client_(sql_client), call_options_(std::move(call_options)),
      metadata_settings_(metadata_settings), connection_(connection) {
  attribute_[METADATA_ID] = static_cast<size_t>(SQL_FALSE);
  attribute_[MAX_LENGTH] = static_cast<size_t>(0);
  attribute_[NOSCAN] = static_cast<size_t>(SQL_NOSCAN_OFF);
  attribute_[QUERY_TIMEOUT] = static_cast<size_t>(0);
  call_options_.timeout = TimeoutDuration{-1};
}

ChargewireStatement::~ChargewireStatement() {
  // Release the stream before the prepared handle. Destructors must not throw.
  if (prepared_statement_ != nullptr) {
    try { if (current_result_set_) current_result_set_->Close(); prepared_statement_->Close(call_options_); } catch (...) {}
    auto status = arrow::Status::OK();
    // Ignore errors during destruction — the statement is going away regardless.
    (void)status;
    prepared_statement_.reset();
  }
}

bool ChargewireStatement::SetAttribute(StatementAttributeId attribute,
                                      const Attribute &value) {
  switch (attribute) {
  case METADATA_ID:
    return CheckIfSetToOnlyValidValue(value, static_cast<size_t>(SQL_FALSE));
  case NOSCAN:
    return CheckIfSetToOnlyValidValue(value, static_cast<size_t>(SQL_NOSCAN_OFF));
  case MAX_LENGTH:
    return CheckIfSetToOnlyValidValue(value, static_cast<size_t>(0));
  case QUERY_TIMEOUT:
    if (boost::get<size_t>(value) > 0) {
      call_options_.timeout =
          TimeoutDuration{static_cast<double>(boost::get<size_t>(value))};
    } else {
      call_options_.timeout = TimeoutDuration{-1};
      // Intentional fall-through.
    }
  default:
    attribute_[attribute] = value;
    return true;
  }
}

boost::optional<Statement::Attribute>
ChargewireStatement::GetAttribute(StatementAttributeId attribute) {
  const auto &it = attribute_.find(attribute);
  return boost::make_optional(it != attribute_.end(), it->second);
}

boost::optional<std::shared_ptr<ResultSetMetadata>>
ChargewireStatement::Prepare(const std::string &query) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  connection_.EnsureTransaction();
  const std::string native_query = TranslateOdbcEscapes(query);
  Result<std::shared_ptr<ChargewirePreparedStatement>> result =
      sql_client_.Prepare(call_options_, native_query, connection_.GetCurrentTransaction());
  ThrowIfNotOK(result.status());

  prepared_statement_ = *result;

  const auto &result_set_metadata =
      std::make_shared<ChargewireResultSetMetadata>(
          prepared_statement_->dataset_schema(), metadata_settings_);
  return boost::optional<std::shared_ptr<ResultSetMetadata>>(
      result_set_metadata);
}

bool ChargewireStatement::ExecutePrepared() {
  assert(prepared_statement_.get() != nullptr);
  // A cached handle can outlive commit/rollback. Establish the next manual txn.
  connection_.EnsureTransaction();
  auto result = prepared_statement_->Execute(call_options_);
  ThrowIfNotOK(result.status());
  flight_info_ = result.ValueOrDie();
  update_count_ = flight_info_->total_records();
  current_result_set_.reset();
  // Chargewire returns a row stream or an update document from one execution.
  // An untyped placeholder may have no result schema until it is bound, so the
  // schema from Prepare must never be used to classify a statement as an update.
  if (flight_info_->schema->num_fields() == 0) return false;
  current_result_set_ = std::make_shared<ChargewireResultSet>(
      sql_client_, call_options_, flight_info_, nullptr, diagnostics_, metadata_settings_);
  return true;
}

bool ChargewireStatement::Execute(const std::string &query) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);
  connection_.EnsureTransaction();
  auto prepared = sql_client_.Prepare(call_options_, TranslateOdbcEscapes(query), connection_.GetCurrentTransaction());
  ThrowIfNotOK(prepared.status());
  prepared_statement_ = *prepared;
  const bool has_rows = ExecutePrepared();
  if (!has_rows) ClosePreparedStatementIfAny(prepared_statement_, call_options_);
  return has_rows;
}

std::shared_ptr<ResultSet> ChargewireStatement::GetResultSet() {
  return current_result_set_;
}

long ChargewireStatement::GetUpdateCount() { return update_count_; }

std::shared_ptr<odbcabstraction::ResultSet> ChargewireStatement::GetTables(
    const std::string *catalog_name, const std::string *schema_name,
    const std::string *table_name, const std::string *table_type,
    const ColumnNames &column_names) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  std::vector<std::string> table_types;

  if ((catalog_name && *catalog_name == "%") &&
      (schema_name && schema_name->empty()) &&
      (table_name && table_name->empty())) {
    current_result_set_ =
        GetTablesForSQLAllCatalogs(
                column_names, call_options_, sql_client_, diagnostics_, metadata_settings_);
  } else if ((catalog_name && catalog_name->empty()) &&
             (schema_name && *schema_name == "%") &&
             (table_name && table_name->empty())) {
    current_result_set_ = GetTablesForSQLAllDbSchemas(
        column_names, call_options_, sql_client_, schema_name, diagnostics_, metadata_settings_);
  } else if ((catalog_name && catalog_name->empty()) &&
             (schema_name && schema_name->empty()) &&
             (table_name && table_name->empty()) &&
             (table_type && *table_type == "%")) {
    current_result_set_ =
        GetTablesForSQLAllTableTypes(
                column_names, call_options_, sql_client_, diagnostics_, metadata_settings_);
  } else {
    if (table_type) {
      ParseTableTypes(*table_type, table_types);
    }

    // ODBC uses "TABLE" for base tables, but SQL standard / DuckDB uses
    // "BASE TABLE".  When callers (e.g. Power Query) request "TABLE",
    // also include "BASE TABLE" so the Flight SQL server returns results.
    if (std::find(table_types.begin(), table_types.end(), "TABLE") != table_types.end()) {
      table_types.emplace_back("BASE TABLE");
    }

    current_result_set_ = GetTablesForGenericUse(
        column_names, call_options_, sql_client_, catalog_name, schema_name,
        table_name, table_types, diagnostics_, metadata_settings_);
  }

  return current_result_set_;
}

std::shared_ptr<ResultSet> ChargewireStatement::GetTables_V2(
    const std::string *catalog_name, const std::string *schema_name,
    const std::string *table_name, const std::string *table_type) {
  ColumnNames column_names{"TABLE_QUALIFIER", "TABLE_OWNER", "TABLE_NAME",
                           "TABLE_TYPE", "REMARKS"};

  return GetTables(catalog_name, schema_name, table_name, table_type,
                   column_names);
}

std::shared_ptr<ResultSet> ChargewireStatement::GetTables_V3(
    const std::string *catalog_name, const std::string *schema_name,
    const std::string *table_name, const std::string *table_type) {
  ColumnNames column_names{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME",
                           "TABLE_TYPE", "REMARKS"};

  return GetTables(catalog_name, schema_name, table_name, table_type,
                   column_names);
}

std::shared_ptr<ResultSet> ChargewireStatement::GetColumns_V2(
    const std::string *catalog_name, const std::string *schema_name,
    const std::string *table_name, const std::string *column_name) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  // Strip ODBC search-pattern escape characters (e.g. "pk\_test" -> "pk_test")
  // before passing to the Flight SQL GetTables RPC, which doesn't understand
  // ODBC-style backslash escaping of wildcards.
  std::string unescaped_table;
  const std::string *effective_table_name = table_name;
  if (table_name) {
    unescaped_table = StripOdbcSearchPatternEscapes(*table_name);
    effective_table_name = &unescaped_table;
  }

  Result<std::shared_ptr<ResultInfo>> result = sql_client_.GetTables(
      call_options_, catalog_name, schema_name, effective_table_name, true, nullptr);
  ThrowIfNotOK(result.status());

  auto flight_info = result.ValueOrDie();

  auto transformer = std::make_shared<GetColumns_Transformer>(
      metadata_settings_, odbcabstraction::V_2, column_name);

  current_result_set_ = std::make_shared<ChargewireResultSet>(
      sql_client_, call_options_, flight_info, transformer, diagnostics_, metadata_settings_);

  return current_result_set_;
}

std::shared_ptr<ResultSet> ChargewireStatement::GetColumns_V3(
    const std::string *catalog_name, const std::string *schema_name,
    const std::string *table_name, const std::string *column_name) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  // Strip ODBC search-pattern escape characters (e.g. "pk\_test" -> "pk_test")
  // before passing to the Flight SQL GetTables RPC, which doesn't understand
  // ODBC-style backslash escaping of wildcards.
  std::string unescaped_table;
  const std::string *effective_table_name = table_name;
  if (table_name) {
    unescaped_table = StripOdbcSearchPatternEscapes(*table_name);
    effective_table_name = &unescaped_table;
  }

  Result<std::shared_ptr<ResultInfo>> result = sql_client_.GetTables(
      call_options_, catalog_name, schema_name, effective_table_name, true, nullptr);
  ThrowIfNotOK(result.status());

  auto flight_info = result.ValueOrDie();

  auto transformer = std::make_shared<GetColumns_Transformer>(
      metadata_settings_, odbcabstraction::V_3, column_name);

  current_result_set_ = std::make_shared<ChargewireResultSet>(
      sql_client_, call_options_, flight_info, transformer, diagnostics_, metadata_settings_);

  return current_result_set_;
}

std::shared_ptr<ResultSet> ChargewireStatement::GetTypeInfo_V2(int16_t data_type) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  Result<std::shared_ptr<ResultInfo>> result = sql_client_.GetXdbcTypeInfo(
          call_options_);
  ThrowIfNotOK(result.status());

  auto flight_info = result.ValueOrDie();

  auto transformer = std::make_shared<GetTypeInfo_Transformer>(
          metadata_settings_, odbcabstraction::V_2, data_type);

  current_result_set_ = std::make_shared<ChargewireResultSet>(
      sql_client_, call_options_, flight_info, transformer, diagnostics_, metadata_settings_);

  return current_result_set_;
}

std::shared_ptr<ResultSet> ChargewireStatement::GetTypeInfo_V3(int16_t data_type) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  Result<std::shared_ptr<ResultInfo>> result = sql_client_.GetXdbcTypeInfo(
          call_options_);
  ThrowIfNotOK(result.status());

  auto flight_info = result.ValueOrDie();

  auto transformer = std::make_shared<GetTypeInfo_Transformer>(
          metadata_settings_, odbcabstraction::V_3, data_type);

  current_result_set_ = std::make_shared<ChargewireResultSet>(
      sql_client_, call_options_, flight_info, transformer, diagnostics_, metadata_settings_);

  return current_result_set_;
}

std::shared_ptr<ResultSet> ChargewireStatement::GetPrimaryKeys(
    const std::string *catalog_name, const std::string *schema_name,
    const std::string *table_name) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  TableRef table_ref;
  if (catalog_name) table_ref.catalog = StripOdbcSearchPatternEscapes(*catalog_name);
  if (schema_name)  table_ref.db_schema = StripOdbcSearchPatternEscapes(*schema_name);
  table_ref.table = table_name ? StripOdbcSearchPatternEscapes(*table_name) : "";

  auto result = sql_client_.GetPrimaryKeys(call_options_, table_ref);
  ThrowIfNotOK(result.status());
  std::shared_ptr<ResultInfo> flight_info(result.ValueOrDie().release());

  std::shared_ptr<arrow::Schema> schema;
  {
    auto schema_result = flight_info->GetSchema(nullptr);
    ThrowIfNotOK(schema_result.status());
    schema = std::move(schema_result).ValueUnsafe();
  }

  auto transformer = RecordBatchTransformerWithTasksBuilder(schema)
                         .RenameField("catalog_name", "TABLE_CAT")
                         .RenameField("db_schema_name", "TABLE_SCHEM")
                         .RenameField("table_name", "TABLE_NAME")
                         .RenameField("column_name", "COLUMN_NAME")
                         .CastField("key_sequence", "KEY_SEQ", arrow::int16())
                         .RenameField("key_name", "PK_NAME")
                         .Build();

  current_result_set_ = std::make_shared<ChargewireResultSet>(
      sql_client_, call_options_, flight_info, transformer, diagnostics_, metadata_settings_);

  return current_result_set_;
}

std::shared_ptr<ResultSet> ChargewireStatement::GetForeignKeys(
    const std::string *pk_catalog_name, const std::string *pk_schema_name,
    const std::string *pk_table_name, const std::string *fk_catalog_name,
    const std::string *fk_schema_name, const std::string *fk_table_name) {
  ClosePreparedStatementIfAny(prepared_statement_, call_options_);

  const bool have_pk = pk_table_name != nullptr;
  const bool have_fk = fk_table_name != nullptr;

  if (!have_pk && !have_fk) {
    // Neither table specified — return empty result set (no RPC to call).
    auto fk_empty_schema = arrow::schema({
      arrow::field("PKTABLE_CAT", arrow::utf8(), true),
      arrow::field("PKTABLE_SCHEM", arrow::utf8(), true),
      arrow::field("PKTABLE_NAME", arrow::utf8(), false),
      arrow::field("PKCOLUMN_NAME", arrow::utf8(), false),
      arrow::field("FKTABLE_CAT", arrow::utf8(), true),
      arrow::field("FKTABLE_SCHEM", arrow::utf8(), true),
      arrow::field("FKTABLE_NAME", arrow::utf8(), false),
      arrow::field("FKCOLUMN_NAME", arrow::utf8(), false),
      arrow::field("KEY_SEQ", arrow::int16(), false),
      arrow::field("UPDATE_RULE", arrow::int16(), true),
      arrow::field("DELETE_RULE", arrow::int16(), true),
      arrow::field("FK_NAME", arrow::utf8(), true),
      arrow::field("PK_NAME", arrow::utf8(), true),
      arrow::field("DEFERRABILITY", arrow::int16(), true)
    });
    auto flight_info = std::make_shared<ResultInfo>(fk_empty_schema);

    current_result_set_ = std::make_shared<ChargewireResultSet>(
        sql_client_, call_options_, flight_info, nullptr, diagnostics_, metadata_settings_);
    return current_result_set_;
  }

  // Build table refs, stripping ODBC escape characters.
  TableRef pk_ref;
  if (pk_catalog_name) pk_ref.catalog = StripOdbcSearchPatternEscapes(*pk_catalog_name);
  if (pk_schema_name)  pk_ref.db_schema = StripOdbcSearchPatternEscapes(*pk_schema_name);
  if (pk_table_name)   pk_ref.table = StripOdbcSearchPatternEscapes(*pk_table_name);

  TableRef fk_ref;
  if (fk_catalog_name) fk_ref.catalog = StripOdbcSearchPatternEscapes(*fk_catalog_name);
  if (fk_schema_name)  fk_ref.db_schema = StripOdbcSearchPatternEscapes(*fk_schema_name);
  if (fk_table_name)   fk_ref.table = StripOdbcSearchPatternEscapes(*fk_table_name);

  // Route to the appropriate RPC based on which tables are specified.
  arrow::Result<std::unique_ptr<ResultInfo>> result;
  if (have_pk && have_fk) {
    result = sql_client_.GetCrossReference(call_options_, pk_ref, fk_ref);
  } else if (have_pk) {
    result = sql_client_.GetExportedKeys(call_options_, pk_ref);
  } else {
    result = sql_client_.GetImportedKeys(call_options_, fk_ref);
  }
  ThrowIfNotOK(result.status());
  std::shared_ptr<ResultInfo> flight_info(result.ValueOrDie().release());

  std::shared_ptr<arrow::Schema> schema;
  {
    auto schema_result = flight_info->GetSchema(nullptr);
    ThrowIfNotOK(schema_result.status());
    schema = std::move(schema_result).ValueUnsafe();
  }

  auto transformer = RecordBatchTransformerWithTasksBuilder(schema)
                         .RenameField("pk_catalog_name", "PKTABLE_CAT")
                         .RenameField("pk_db_schema_name", "PKTABLE_SCHEM")
                         .RenameField("pk_table_name", "PKTABLE_NAME")
                         .RenameField("pk_column_name", "PKCOLUMN_NAME")
                         .RenameField("fk_catalog_name", "FKTABLE_CAT")
                         .RenameField("fk_db_schema_name", "FKTABLE_SCHEM")
                         .RenameField("fk_table_name", "FKTABLE_NAME")
                         .RenameField("fk_column_name", "FKCOLUMN_NAME")
                         .CastField("key_sequence", "KEY_SEQ", arrow::int16())
                         .CastField("update_rule", "UPDATE_RULE", arrow::int16())
                         .CastField("delete_rule", "DELETE_RULE", arrow::int16())
                         .RenameField("fk_key_name", "FK_NAME")
                         .RenameField("pk_key_name", "PK_NAME")
                         .AddFieldOfNulls("DEFERRABILITY", arrow::int16())
                         .Build();

  current_result_set_ = std::make_shared<ChargewireResultSet>(
      sql_client_, call_options_, flight_info, transformer, diagnostics_, metadata_settings_);

  return current_result_set_;
}

std::shared_ptr<ResultSetMetadata> ChargewireStatement::GetParameterSchema() {
  if (!prepared_statement_) {
    return nullptr;
  }

  auto param_schema = prepared_statement_->parameter_schema();
  if (!param_schema || param_schema->num_fields() == 0) {
    return nullptr;
  }

  return std::make_shared<ChargewireParameterMetadata>(param_schema, metadata_settings_);
}

void ChargewireStatement::SetParameters(const odbcabstraction::ParameterSet& params) {
  if (!prepared_statement_) {
    throw DriverException("Statement is not prepared", "HY010");
  }

  auto param_schema = prepared_statement_->parameter_schema();
  if (!param_schema) {
    throw DriverException("Prepared statement has no parameter schema", "07002");
  }

  // One row per parameter set: Chargewire runs each row as one execution.
  ThrowIfNotOK(prepared_statement_->SetParameters(BuildParameterBatch(param_schema, params)));
}

odbcabstraction::Diagnostics &ChargewireStatement::GetDiagnostics() {
  return diagnostics_;
}

void ChargewireStatement::Cancel() {
  // The native client's cancellation channel is safe from another thread.
  // The executing/fetching thread owns the result set and reports cancellation.
  sql_client_.Cancel();
}

} // namespace chargewire
} // namespace driver
