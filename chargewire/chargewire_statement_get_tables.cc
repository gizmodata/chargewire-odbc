/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "chargewire_statement_get_tables.h"
#include <odbcabstraction/platform.h>
#include "chargewire_client.h"
#include "chargewire_result_set.h"
#include "record_batch_transformer.h"
#include "utils.h"
#include <arrow/builder.h>
#include <arrow/compute/api.h>
#include <arrow/table.h>

namespace driver {
namespace chargewire_odbc {

using arrow::Result;

void ParseTableTypes(const std::string &table_type,
                     std::vector<std::string> &table_types) {
  bool encountered = false; // for checking if there is a single quote
  std::string curr_parse;   // the current string

  for (char temp : table_type) { // while still in the string
    switch (temp) {              // switch depending on the character
    case '\'':                   // if the character is a single quote
      if (encountered) {
        encountered =
            false; // if we already found a single quote, reset encountered
      } else {
        encountered =
            true; // if we haven't found a single quote, set encountered to true
      }
      break;
    case ',':             // if it is a comma
      if (!encountered) { // if we have not found a single quote
        table_types.push_back(
            curr_parse); // put our current string into our vector
        curr_parse = ""; // reset the current string
        break;
      }
    default: // if it is a normal character
      if (encountered && isspace(temp)) {
        curr_parse.push_back(temp); // if we have found a single quote put the
                                    // whitespace, we don't care
      } else if (temp == '\'' || temp == ' ') {
        break; // if the current character is a single quote, trash it and go to
               // the next character.
      } else {
        curr_parse.push_back(temp); // if all of the above failed, put the
                                    // character into the current string
      }
      break; // go to the next character
    }
  }
  table_types.emplace_back(
      curr_parse); // if we have found a single quote put the whitespace,
  // we don't care
}

std::shared_ptr<ResultSet>
GetTablesForSQLAllCatalogs(const ColumnNames &names,
                           CallOptions &call_options,
                           ChargewireClient &sql_client,
                           odbcabstraction::Diagnostics &diagnostics,
                           const odbcabstraction::MetadataSettings &metadata_settings) {
  Result<std::shared_ptr<ResultInfo>> result =
      sql_client.GetCatalogs(call_options);

  std::shared_ptr<Schema> schema;
  std::shared_ptr<ResultInfo> flight_info;

  ThrowIfNotOK(result.status());
  flight_info = result.ValueOrDie();
  {
    auto schema_result = flight_info->GetSchema(nullptr);
    ThrowIfNotOK(schema_result.status());
    schema = std::move(schema_result).ValueUnsafe();
  }

  auto transformer = RecordBatchTransformerWithTasksBuilder(schema)
                         .RenameField("catalog_name", names.catalog_column)
                         .AddFieldOfNulls(names.schema_column, utf8())
                         .AddFieldOfNulls(names.table_column, utf8())
                         .AddFieldOfNulls(names.table_type_column, utf8())
                         .AddFieldOfNulls(names.remarks_column, utf8())
                         .Build();

  return std::make_shared<ChargewireResultSet>(sql_client, call_options,
                                              flight_info, transformer, diagnostics, metadata_settings);
}

std::shared_ptr<ResultSet> GetTablesForSQLAllDbSchemas(
    const ColumnNames &names, CallOptions &call_options,
    ChargewireClient &sql_client, const std::string *schema_name,
    odbcabstraction::Diagnostics &diagnostics, const odbcabstraction::MetadataSettings &metadata_settings) {
  Result<std::shared_ptr<ResultInfo>> result =
      sql_client.GetDbSchemas(call_options, nullptr, schema_name);

  std::shared_ptr<Schema> schema;
  std::shared_ptr<ResultInfo> flight_info;

  ThrowIfNotOK(result.status());
  flight_info = result.ValueOrDie();
  {
    auto schema_result = flight_info->GetSchema(nullptr);
    ThrowIfNotOK(schema_result.status());
    schema = std::move(schema_result).ValueUnsafe();
  }

  auto transformer = RecordBatchTransformerWithTasksBuilder(schema)
                         .AddFieldOfNulls(names.catalog_column, utf8())
                         .RenameField("db_schema_name", names.schema_column)
                         .AddFieldOfNulls(names.table_column, utf8())
                         .AddFieldOfNulls(names.table_type_column, utf8())
                         .AddFieldOfNulls(names.remarks_column, utf8())
                         .Build();

  return std::make_shared<ChargewireResultSet>(sql_client, call_options,
                                              flight_info, transformer, diagnostics, metadata_settings);
}

std::shared_ptr<ResultSet>
GetTablesForSQLAllTableTypes(const ColumnNames &names,
                             CallOptions &call_options,
                             ChargewireClient &sql_client,
                             odbcabstraction::Diagnostics &diagnostics,
                             const odbcabstraction::MetadataSettings &metadata_settings) {
  Result<std::shared_ptr<ResultInfo>> result =
      sql_client.GetTableTypes(call_options);

  std::shared_ptr<Schema> schema;
  std::shared_ptr<ResultInfo> flight_info;

  ThrowIfNotOK(result.status());
  flight_info = result.ValueOrDie();
  {
    auto schema_result = flight_info->GetSchema(nullptr);
    ThrowIfNotOK(schema_result.status());
    schema = std::move(schema_result).ValueUnsafe();
  }

  auto transformer = RecordBatchTransformerWithTasksBuilder(schema)
                         .AddFieldOfNulls(names.catalog_column, utf8())
                         .AddFieldOfNulls(names.schema_column, utf8())
                         .AddFieldOfNulls(names.table_column, utf8())
                         .ReplaceFieldValues("table_type", names.table_type_column,
                                             {{"BASE TABLE", "TABLE"}})
                         .AddFieldOfNulls(names.remarks_column, utf8())
                         .Build();

  return std::make_shared<ChargewireResultSet>(sql_client, call_options,
                                              flight_info, transformer, diagnostics, metadata_settings);
}

std::shared_ptr<ResultInfo>
FilterTablesByCatalogPattern(const std::shared_ptr<ResultInfo> &tables,
                             const std::string &catalog_pattern) {
  std::shared_ptr<arrow::Table> table;
  if (tables->reader) {
    auto read = tables->reader->ToTable();
    ThrowIfNotOK(read.status());
    table = std::move(read).ValueUnsafe();
  } else {
    auto empty = arrow::Table::MakeEmpty(tables->schema);
    ThrowIfNotOK(empty.status());
    table = std::move(empty).ValueUnsafe();
  }

  const auto catalogs = table->GetColumnByName("catalog_name");
  if (!catalogs) {
    throw odbcabstraction::DriverException(
        "The server's tables listing has no catalog_name column", "HY000");
  }
  arrow::BooleanBuilder keep;
  ThrowIfNotOK(keep.Reserve(table->num_rows()));
  for (const auto &chunk : catalogs->chunks()) {
    auto append = [&](const auto &names) {
      for (int64_t row = 0; row < names.length(); ++row) {
        keep.UnsafeAppend(names.IsValid(row) &&
                          MatchesOdbcSearchPattern(names.GetView(row), catalog_pattern));
      }
    };
    switch (chunk->type_id()) {
    case arrow::Type::STRING:
      append(static_cast<const arrow::StringArray &>(*chunk));
      break;
    case arrow::Type::LARGE_STRING:
      append(static_cast<const arrow::LargeStringArray &>(*chunk));
      break;
    case arrow::Type::STRING_VIEW:
      append(static_cast<const arrow::StringViewArray &>(*chunk));
      break;
    case arrow::Type::NA:
      for (int64_t row = 0; row < chunk->length(); ++row) keep.UnsafeAppend(false);
      break;
    default:
      throw odbcabstraction::DriverException(
          "The server's tables listing has a catalog_name column of type " +
              chunk->type()->ToString(), "HY000");
    }
  }
  std::shared_ptr<arrow::Array> mask;
  ThrowIfNotOK(keep.Finish(&mask));
  auto filtered = arrow::compute::Filter(table, mask);
  ThrowIfNotOK(filtered.status());
  auto kept = filtered.ValueUnsafe().table();

  auto info = std::make_shared<ResultInfo>(tables->schema);
  info->health = tables->health;
  info->reader = std::make_shared<arrow::TableBatchReader>(kept);
  info->rows = kept->num_rows();
  return info;
}

std::shared_ptr<ResultSet> GetTablesForGenericUse(
    const ColumnNames &names, CallOptions &call_options,
    ChargewireClient &sql_client, const std::string *catalog_name,
    const std::string *schema_name, const std::string *table_name,
    const std::vector<std::string> &table_types, bool catalog_is_pattern,
    odbcabstraction::Diagnostics &diagnostics, const odbcabstraction::MetadataSettings &metadata_settings) {
  // The server takes the catalog as an exact name (spec: `catalog`, unlike the schema and
  // table filter patterns). An ODBC 3 catalog pattern without wildcards is that name,
  // unescaped; one of only '%' is every catalog; any other lists every catalog's tables
  // and keeps the matching rows here. Sent as is, "%" matched no catalog (issue #6 of
  // hardcharger-odbc-driver: Power Query's Navigator showed no tables).
  const std::string *server_catalog = catalog_name;
  std::string exact_catalog;
  const std::string *catalog_pattern = nullptr;
  if (catalog_name && catalog_is_pattern) {
    auto search = AnalyzeOdbcSearchPattern(*catalog_name);
    switch (search.kind) {
    case OdbcSearchPattern::kExact:
      exact_catalog = std::move(search.literal);
      server_catalog = &exact_catalog;
      break;
    case OdbcSearchPattern::kAll:
      server_catalog = nullptr;
      break;
    case OdbcSearchPattern::kWildcard:
      server_catalog = nullptr;
      catalog_pattern = catalog_name;
      break;
    }
  }

  // Pass nullptr when table_types is empty (no filter specified) so the
  // Flight SQL server returns all table types.  A non-null pointer to an
  // empty vector means "match no types" in the Flight SQL protocol.
  Result<std::shared_ptr<ResultInfo>> result = sql_client.GetTables(
      call_options, server_catalog, schema_name, table_name, false,
      table_types.empty() ? nullptr : &table_types);

  std::shared_ptr<Schema> schema;
  std::shared_ptr<ResultInfo> flight_info;

  ThrowIfNotOK(result.status());
  flight_info = result.ValueOrDie();
  if (catalog_pattern) {
    flight_info = FilterTablesByCatalogPattern(flight_info, *catalog_pattern);
  }
  {
    auto schema_result = flight_info->GetSchema(nullptr);
    ThrowIfNotOK(schema_result.status());
    schema = std::move(schema_result).ValueUnsafe();
  }

  if (metadata_settings.hide_sql_tables_listing_) {
    auto transformer = RecordBatchTransformerWithTasksBuilder(schema)
     .AddFieldOfNulls(names.remarks_column, utf8())
     .Build();

    return std::make_shared<ChargewireResultSet>(
      sql_client, call_options, flight_info,
      transformer, diagnostics, metadata_settings
    );
  }
  else {
    auto transformer = RecordBatchTransformerWithTasksBuilder(schema)
     .RenameField("catalog_name", names.catalog_column)
     .RenameField("db_schema_name", names.schema_column)
     .RenameField("table_name", names.table_column)
     .ReplaceFieldValues("table_type", names.table_type_column,
                         {{"BASE TABLE", "TABLE"}})
     .AddFieldOfNulls(names.remarks_column, utf8())
     .Build();

    return std::make_shared<ChargewireResultSet>(
      sql_client, call_options, flight_info,
      transformer, diagnostics, metadata_settings
    );
  }
}

} // namespace chargewire
} // namespace driver
