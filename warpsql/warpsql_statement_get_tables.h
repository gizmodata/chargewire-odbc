/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include "warpsql_connection.h"
#include "warp_client.h"
#include <odbcabstraction/spi/result_set.h>
#include <odbcabstraction/diagnostics.h>
#include "record_batch_transformer.h"
#include "odbcabstraction/types.h"
#include "warp_client.h"
#include <arrow/type.h>

namespace driver {
namespace warpsql_odbc {

using odbcabstraction::ResultSet;
using odbcabstraction::MetadataSettings;

typedef struct {
  std::string catalog_column;
  std::string schema_column;
  std::string table_column;
  std::string table_type_column;
  std::string remarks_column;
} ColumnNames;

void ParseTableTypes(const std::string &table_type,
                     std::vector<std::string> &table_types);

std::shared_ptr<ResultSet>
GetTablesForSQLAllCatalogs(const ColumnNames &column_names,
                           CallOptions &call_options,
                           WarpSqlClient &sql_client,
                           odbcabstraction::Diagnostics &diagnostics,
                           const odbcabstraction::MetadataSettings &metadata_settings);

std::shared_ptr<ResultSet> GetTablesForSQLAllDbSchemas(
    const ColumnNames &column_names, CallOptions &call_options,
    WarpSqlClient &sql_client, const std::string *schema_name,
    odbcabstraction::Diagnostics &diagnostics, const odbcabstraction::MetadataSettings &metadata_settings);

std::shared_ptr<ResultSet>
GetTablesForSQLAllTableTypes(const ColumnNames &column_names,
                             CallOptions &call_options,
                             WarpSqlClient &sql_client,
                             odbcabstraction::Diagnostics &diagnostics,
                             const odbcabstraction::MetadataSettings &metadata_settings);

std::shared_ptr<ResultSet> GetTablesForGenericUse(
    const ColumnNames &column_names, CallOptions &call_options,
    WarpSqlClient &sql_client, const std::string *catalog_name,
    const std::string *schema_name, const std::string *table_name,
    const std::vector<std::string> &table_types,
    odbcabstraction::Diagnostics &diagnostics,
    const odbcabstraction::MetadataSettings &metadata_settings);
} // namespace warpsql
} // namespace driver
