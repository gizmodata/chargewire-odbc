/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "get_info_cache.h"

#include <odbcabstraction/platform.h>

#include <arrow/array.h>
#include <arrow/array/array_nested.h>
#include "warp_client.h"
#include <arrow/scalar.h>
#include <arrow/type_fwd.h>
#include <odbcabstraction/exceptions.h>
#include <sql.h>
#include <sqlext.h>

#include "hardcharger_stream_chunk_buffer.h"
#include "scalar_function_reporter.h"
#include "utils.h"

// Aliases for entries in SqlInfoOptions::SqlInfo that are defined here
// due to causing compilation errors conflicting with ODBC definitions.
#define ARROW_SQL_IDENTIFIER_CASE 503
#define ARROW_SQL_IDENTIFIER_QUOTE_CHAR 504
#define ARROW_SQL_QUOTED_IDENTIFIER_CASE 505
#define ARROW_SQL_KEYWORDS 508
#define ARROW_SQL_NUMERIC_FUNCTIONS 509
#define ARROW_SQL_STRING_FUNCTIONS 510
#define ARROW_SQL_SYSTEM_FUNCTIONS 511
#define ARROW_SQL_SCHEMA_TERM 529
#define ARROW_SQL_PROCEDURE_TERM 530
#define ARROW_SQL_CATALOG_TERM 531
#define ARROW_SQL_MAX_COLUMNS_IN_GROUP_BY 544
#define ARROW_SQL_MAX_COLUMNS_IN_INDEX 545
#define ARROW_SQL_MAX_COLUMNS_IN_ORDER_BY 546
#define ARROW_SQL_MAX_COLUMNS_IN_SELECT 547
#define ARROW_SQL_MAX_COLUMNS_IN_TABLE 548
#define ARROW_SQL_MAX_ROW_SIZE 555
#define ARROW_SQL_MAX_TABLES_IN_SELECT 560

#define ARROW_CONVERT_BIGINT 0
#define ARROW_CONVERT_BINARY 1
#define ARROW_CONVERT_BIT 2
#define ARROW_CONVERT_CHAR 3
#define ARROW_CONVERT_DATE 4
#define ARROW_CONVERT_DECIMAL 5
#define ARROW_CONVERT_FLOAT 6
#define ARROW_CONVERT_INTEGER 7
#define ARROW_CONVERT_INTERVAL_DAY_TIME 8
#define ARROW_CONVERT_INTERVAL_YEAR_MONTH 9
#define ARROW_CONVERT_LONGVARBINARY 10
#define ARROW_CONVERT_LONGVARCHAR 11
#define ARROW_CONVERT_NUMERIC 12
#define ARROW_CONVERT_REAL 13
#define ARROW_CONVERT_SMALLINT 14
#define ARROW_CONVERT_TIME 15
#define ARROW_CONVERT_TIMESTAMP 16
#define ARROW_CONVERT_TINYINT 17
#define ARROW_CONVERT_VARBINARY 18
#define ARROW_CONVERT_VARCHAR 19

namespace {
// Return the corresponding field in SQLGetInfo's SQL_CONVERT_* field
// types for the given Arrow SqlConvert enum value.
//
// The caller is responsible for casting the result to a uint16. Note
// that -1 is returned if there's no corresponding entry.
int32_t GetInfoTypeForArrowConvertEntry(int32_t convert_entry) {
  switch (convert_entry) {
  case ARROW_CONVERT_BIGINT:
    return SQL_CONVERT_BIGINT;
  case ARROW_CONVERT_BINARY:
    return SQL_CONVERT_BINARY;
  case ARROW_CONVERT_BIT:
    return SQL_CONVERT_BIT;
  case ARROW_CONVERT_CHAR:
    return SQL_CONVERT_CHAR;
  case ARROW_CONVERT_DATE:
    return SQL_CONVERT_DATE;
  case ARROW_CONVERT_DECIMAL:
    return SQL_CONVERT_DECIMAL;
  case ARROW_CONVERT_FLOAT:
    return SQL_CONVERT_FLOAT;
  case ARROW_CONVERT_INTEGER:
    return SQL_CONVERT_INTEGER;
  case ARROW_CONVERT_INTERVAL_DAY_TIME:
    return SQL_CONVERT_INTERVAL_DAY_TIME;
  case ARROW_CONVERT_INTERVAL_YEAR_MONTH:
    return SQL_CONVERT_INTERVAL_YEAR_MONTH;
  case ARROW_CONVERT_LONGVARBINARY:
    return SQL_CONVERT_LONGVARBINARY;
  case ARROW_CONVERT_LONGVARCHAR:
    return SQL_CONVERT_LONGVARCHAR;
  case ARROW_CONVERT_NUMERIC:
    return SQL_CONVERT_NUMERIC;
  case ARROW_CONVERT_REAL:
    return SQL_CONVERT_REAL;
  case ARROW_CONVERT_SMALLINT:
    return SQL_CONVERT_SMALLINT;
  case ARROW_CONVERT_TIME:
    return SQL_CONVERT_TIME;
  case ARROW_CONVERT_TIMESTAMP:
    return SQL_CONVERT_TIMESTAMP;
  case ARROW_CONVERT_TINYINT:
    return SQL_CONVERT_TINYINT;
  case ARROW_CONVERT_VARBINARY:
    return SQL_CONVERT_VARBINARY;
  case ARROW_CONVERT_VARCHAR:
    return SQL_CONVERT_VARCHAR;
  }
  // Arbitrarily return a negative value
  return -1;
}

// Return the corresponding bitmask to OR in SQLGetInfo's SQL_CONVERT_* field
// value for the given Arrow SqlConvert enum value.
//
// This is _not_ a bit position, it is an integer with only a single bit set.
uint32_t GetCvtBitForArrowConvertEntry(int32_t convert_entry) {
  switch (convert_entry) {
  case ARROW_CONVERT_BIGINT:
    return SQL_CVT_BIGINT;
  case ARROW_CONVERT_BINARY:
    return SQL_CVT_BINARY;
  case ARROW_CONVERT_BIT:
    return SQL_CVT_BIT;
  case ARROW_CONVERT_CHAR:
    return SQL_CVT_CHAR | SQL_CVT_WCHAR;
  case ARROW_CONVERT_DATE:
    return SQL_CVT_DATE;
  case ARROW_CONVERT_DECIMAL:
    return SQL_CVT_DECIMAL;
  case ARROW_CONVERT_FLOAT:
    return SQL_CVT_FLOAT;
  case ARROW_CONVERT_INTEGER:
    return SQL_CVT_INTEGER;
  case ARROW_CONVERT_INTERVAL_DAY_TIME:
    return SQL_CVT_INTERVAL_DAY_TIME;
  case ARROW_CONVERT_INTERVAL_YEAR_MONTH:
    return SQL_CVT_INTERVAL_YEAR_MONTH;
  case ARROW_CONVERT_LONGVARBINARY:
    return SQL_CVT_LONGVARBINARY;
  case ARROW_CONVERT_LONGVARCHAR:
    return SQL_CVT_LONGVARCHAR | SQL_CVT_WLONGVARCHAR;
  case ARROW_CONVERT_NUMERIC:
    return SQL_CVT_NUMERIC;
  case ARROW_CONVERT_REAL:
    return SQL_CVT_REAL;
  case ARROW_CONVERT_SMALLINT:
    return SQL_CVT_SMALLINT;
  case ARROW_CONVERT_TIME:
    return SQL_CVT_TIME;
  case ARROW_CONVERT_TIMESTAMP:
    return SQL_CVT_TIMESTAMP;
  case ARROW_CONVERT_TINYINT:
    return SQL_CVT_TINYINT;
  case ARROW_CONVERT_VARBINARY:
    return SQL_CVT_VARBINARY;
  case ARROW_CONVERT_VARCHAR:
    return SQL_CVT_VARCHAR | SQL_CVT_WLONGVARCHAR;
  }
  // Note: GUID not supported by GetSqlInfo.
  // Return zero, which has no bits set.
  return 0;
}

inline int32_t ScalarToInt32(arrow::UnionScalar *scalar) {
  return reinterpret_cast<arrow::Int32Scalar *>(scalar->child_value().get())->value;
}

inline int64_t ScalarToInt64(arrow::UnionScalar *scalar) {
  return reinterpret_cast<arrow::Int64Scalar *>(scalar->child_value().get())->value;
}

inline std::string ScalarToBoolString(arrow::UnionScalar *scalar) {
  return reinterpret_cast<arrow::BooleanScalar *>(scalar->child_value().get())->value ? "Y" : "N";
}

inline void SetDefaultIfMissing(std::unordered_map<uint16_t, driver::odbcabstraction::Connection::Info>& cache,
                                uint16_t info_type, driver::odbcabstraction::Connection::Info default_value) {
  // Note: emplace() only writes if the key isn't found.
  cache.emplace(info_type, std::move(default_value));
}

} // namespace

namespace driver {
namespace hardcharger {


using namespace driver::odbcabstraction;

GetInfoCache::GetInfoCache(CallOptions &call_options,
                           std::unique_ptr<WarpSqlClient> &client, const std::string &driver_version)
    : call_options_(call_options), sql_client_(client),
      has_server_info_(false) {
  info_[SQL_DRIVER_NAME] = "Hardcharger ODBC Driver";
  info_[SQL_DRIVER_VER] = ConvertToDBMSVer(driver_version);

  info_[SQL_GETDATA_EXTENSIONS] =
      static_cast<uint32_t>(SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER);
  info_[SQL_CURSOR_SENSITIVITY] = static_cast<uint32_t>(SQL_UNSPECIFIED);

  // Properties which don't currently have SqlGetInfo fields but probably
  // should.
  info_[SQL_ACCESSIBLE_PROCEDURES] = "N";
  info_[SQL_COLLATION_SEQ] = "";
  info_[SQL_ALTER_DOMAIN] = static_cast<uint32_t>(0);
  info_[SQL_ALTER_TABLE] = static_cast<uint32_t>(0);
  info_[SQL_COLUMN_ALIAS] = "Y";
  info_[SQL_DATETIME_LITERALS] = static_cast<uint32_t>(
      SQL_DL_SQL92_DATE | SQL_DL_SQL92_TIME | SQL_DL_SQL92_TIMESTAMP);
  info_[SQL_CREATE_ASSERTION] = static_cast<uint32_t>(0);
  info_[SQL_CREATE_CHARACTER_SET] = static_cast<uint32_t>(0);
  info_[SQL_CREATE_COLLATION] = static_cast<uint32_t>(0);
  info_[SQL_CREATE_DOMAIN] = static_cast<uint32_t>(0);
  info_[SQL_INDEX_KEYWORDS] = static_cast<uint32_t>(SQL_IK_NONE);
  info_[SQL_TIMEDATE_ADD_INTERVALS] = static_cast<uint32_t>(
      SQL_FN_TSI_FRAC_SECOND | SQL_FN_TSI_SECOND | SQL_FN_TSI_MINUTE |
      SQL_FN_TSI_HOUR | SQL_FN_TSI_DAY | SQL_FN_TSI_WEEK | SQL_FN_TSI_MONTH |
      SQL_FN_TSI_QUARTER | SQL_FN_TSI_YEAR);
  info_[SQL_TIMEDATE_DIFF_INTERVALS] = static_cast<uint32_t>(
      SQL_FN_TSI_FRAC_SECOND | SQL_FN_TSI_SECOND | SQL_FN_TSI_MINUTE |
      SQL_FN_TSI_HOUR | SQL_FN_TSI_DAY | SQL_FN_TSI_WEEK | SQL_FN_TSI_MONTH |
      SQL_FN_TSI_QUARTER | SQL_FN_TSI_YEAR);
  info_[SQL_CURSOR_COMMIT_BEHAVIOR] = static_cast<uint16_t>(SQL_CB_CLOSE);
  info_[SQL_CURSOR_ROLLBACK_BEHAVIOR] = static_cast<uint16_t>(SQL_CB_CLOSE);
  info_[SQL_CREATE_TRANSLATION] = static_cast<uint32_t>(0);
  info_[SQL_DDL_INDEX] = static_cast<uint32_t>(0);
  info_[SQL_DROP_ASSERTION] = static_cast<uint32_t>(0);
  info_[SQL_DROP_CHARACTER_SET] = static_cast<uint32_t>(0);
  info_[SQL_DROP_COLLATION] = static_cast<uint32_t>(0);
  info_[SQL_DROP_DOMAIN] = static_cast<uint32_t>(0);
  info_[SQL_DROP_SCHEMA] = static_cast<uint32_t>(0);
  info_[SQL_DROP_TABLE] = static_cast<uint32_t>(0);
  info_[SQL_DROP_TRANSLATION] = static_cast<uint32_t>(0);
  info_[SQL_DROP_VIEW] = static_cast<uint32_t>(0);
  info_[SQL_MAX_IDENTIFIER_LEN] = static_cast<uint16_t>(65535); // arbitrary

  // Assume all aggregate functions reported in ODBC are supported.
  info_[SQL_AGGREGATE_FUNCTIONS] = static_cast<uint32_t>(
      SQL_AF_ALL | SQL_AF_AVG | SQL_AF_COUNT | SQL_AF_DISTINCT | SQL_AF_MAX |
      SQL_AF_MIN | SQL_AF_SUM);

  // Assume catalogs are not supported by default. ODBC checks if SQL_CATALOG_NAME is
  // "Y" or "N" to determine if catalogs are supported.
  info_[SQL_CATALOG_TERM] = "";
  info_[SQL_CATALOG_NAME] = "N";
  info_[SQL_CATALOG_NAME_SEPARATOR] = "";
  info_[SQL_CATALOG_LOCATION] = static_cast<uint16_t>(0);
  LoadDefaultsForMissingEntries();
  info_[SQL_CATALOG_NAME] = "Y";
  info_[SQL_CATALOG_TERM] = "catalog";
  info_[SQL_CATALOG_NAME_SEPARATOR] = ".";
  info_[SQL_CATALOG_LOCATION] = static_cast<uint16_t>(SQL_CL_START);
  info_[SQL_CATALOG_USAGE] = static_cast<uint32_t>(SQL_CU_DML_STATEMENTS | SQL_CU_TABLE_DEFINITION | SQL_CU_INDEX_DEFINITION);
  info_[SQL_TXN_CAPABLE] = static_cast<uint16_t>(SQL_TC_ALL);
  info_[SQL_DEFAULT_TXN_ISOLATION] = static_cast<uint32_t>(SQL_TXN_SERIALIZABLE);
  info_[SQL_TXN_ISOLATION_OPTION] = static_cast<uint32_t>(SQL_TXN_SERIALIZABLE);
  info_[SQL_DESCRIBE_PARAMETER] = "Y";
  info_[SQL_DBMS_NAME] = "Hardcharger";
  info_[SQL_DBMS_VER] = "02.00.0000";
  info_[SQL_ODBC_INTERFACE_CONFORMANCE] = static_cast<uint32_t>(SQL_OIC_CORE);
  info_[SQL_SQL_CONFORMANCE] = static_cast<uint32_t>(SQL_SC_SQL92_ENTRY);
  info_[SQL_MAX_CONCURRENT_ACTIVITIES] = static_cast<uint16_t>(0);

}

void GetInfoCache::SetProperty(
    uint16_t property, driver::odbcabstraction::Connection::Info value) {
  info_[property] = value;
}

Connection::Info GetInfoCache::GetInfo(uint16_t info_type) {
  if (sql_client_ && !has_server_info_) LoadInfoFromServer();
  auto it = info_.find(info_type);

  if (info_.end() == it) {
    if (LoadInfoFromServer()) {
      it = info_.find(info_type);
    }
    if (info_.end() == it) {
      throw DriverException("Unknown GetInfo type: " +
                            std::to_string(info_type));
    }
  }
  return it->second;
}

bool GetInfoCache::LoadInfoFromServer() {
  if (sql_client_ && !has_server_info_.exchange(true)) {
    std::unique_lock<std::mutex> lock(mutex_);

    // Wrap server call in try-catch so defaults are always loaded even
    // if GetSqlInfo fails. Without this, has_server_info_ would be true
    // but no defaults would be set, causing all GetInfo calls to fail.
    try {
    auto server = sql_client_->ServerInfo();
    auto text = [&](const char* key, uint16_t target) {
      if (auto* v = server.if_contains(key); v && v->is_string())
        info_[target] = std::string(v->as_string());
    };
    text("server_name", SQL_DBMS_NAME);
    text("server_name", SQL_SERVER_NAME);
    if (auto* v = server.if_contains("server_version"); v && v->is_string())
      info_[SQL_DBMS_VER] = ConvertToDBMSVer(std::string(v->as_string()));
    text("sql_identifier_quote_char", SQL_IDENTIFIER_QUOTE_CHAR);
    text("sql_search_string_escape", SQL_SEARCH_PATTERN_ESCAPE);
    text("sql_schema_term", SQL_SCHEMA_TERM);
    text("sql_catalog_term", SQL_CATALOG_TERM);
    } catch (const std::exception &e) {
      // Log but don't propagate — defaults will fill in the gaps.
      (void)e;
    }
    LoadDefaultsForMissingEntries();
    return true;
  }

  return false;
}

void GetInfoCache::LoadDefaultsForMissingEntries() {
  // For safety's sake, this function does not discriminate between driver and hard-coded values.
  SetDefaultIfMissing(info_, SQL_ACCESSIBLE_PROCEDURES, "N");
  SetDefaultIfMissing(info_, SQL_ACCESSIBLE_TABLES, "Y");
  SetDefaultIfMissing(info_, SQL_ACTIVE_ENVIRONMENTS, static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_AGGREGATE_FUNCTIONS,
                      static_cast<uint32_t>(SQL_AF_ALL | SQL_AF_AVG |
                                            SQL_AF_COUNT | SQL_AF_DISTINCT |
                                            SQL_AF_MAX | SQL_AF_MIN |
                                            SQL_AF_SUM));
  SetDefaultIfMissing(info_, SQL_ALTER_DOMAIN, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_ALTER_TABLE, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_ASYNC_MODE,
                      static_cast<uint32_t>(SQL_AM_NONE));
  SetDefaultIfMissing(info_, SQL_BATCH_ROW_COUNT, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_BATCH_SUPPORT, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_BOOKMARK_PERSISTENCE,
                      static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CATALOG_LOCATION, static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_CATALOG_NAME, "N");
  SetDefaultIfMissing(info_, SQL_CATALOG_NAME_SEPARATOR, "");
  SetDefaultIfMissing(info_, SQL_CATALOG_TERM, "");
  SetDefaultIfMissing(info_, SQL_CATALOG_USAGE, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_COLLATION_SEQ, "");
  SetDefaultIfMissing(info_, SQL_COLUMN_ALIAS, "Y");
  SetDefaultIfMissing(info_, SQL_CONCAT_NULL_BEHAVIOR,
                      static_cast<uint16_t>(SQL_CB_NULL));
  SetDefaultIfMissing(info_, SQL_CONVERT_BIGINT, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_BINARY, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_BIT, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_CHAR, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_DATE, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_DECIMAL, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_DOUBLE, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_FLOAT, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_GUID, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_INTEGER,static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_INTERVAL_YEAR_MONTH,
                      static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_INTERVAL_DAY_TIME,
                      static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_LONGVARBINARY,
                      static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_LONGVARCHAR, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_NUMERIC, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_REAL, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_SMALLINT, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_TIME, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_TIMESTAMP, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_TINYINT, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_VARBINARY, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_VARCHAR, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_WCHAR, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_WVARCHAR, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_WLONGVARCHAR,
                      static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CONVERT_WLONGVARCHAR,
                      static_cast<uint32_t>(SQL_FN_CVT_CAST));
  SetDefaultIfMissing(info_, SQL_CORRELATION_NAME,
                      static_cast<uint32_t>(SQL_CN_NONE));
  SetDefaultIfMissing(info_, SQL_CREATE_ASSERTION, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CREATE_CHARACTER_SET,
                      static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CREATE_DOMAIN, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CREATE_SCHEMA, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CREATE_TABLE, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CREATE_TRANSLATION, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CREATE_VIEW, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_CURSOR_COMMIT_BEHAVIOR,
                      static_cast<uint16_t>(SQL_CB_CLOSE));
  SetDefaultIfMissing(info_, SQL_CURSOR_ROLLBACK_BEHAVIOR,
                      static_cast<uint16_t>(SQL_CB_CLOSE));
  SetDefaultIfMissing(info_, SQL_CURSOR_SENSITIVITY,
                      static_cast<uint32_t>(SQL_UNSPECIFIED));
  SetDefaultIfMissing(info_, SQL_DATA_SOURCE_READ_ONLY, "N");
  SetDefaultIfMissing(info_, SQL_DBMS_NAME, "Hardcharger");
  SetDefaultIfMissing(info_, SQL_DBMS_VER, "00.01.0000");
  SetDefaultIfMissing(info_, SQL_DDL_INDEX, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_DEFAULT_TXN_ISOLATION,
                      static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_DESCRIBE_PARAMETER, "N");
  SetDefaultIfMissing(info_, SQL_DRIVER_NAME, "Hardcharger ODBC Driver");
  SetDefaultIfMissing(info_, SQL_DRIVER_ODBC_VER, "03.80");
  SetDefaultIfMissing(info_, SQL_DRIVER_VER, "00.09.0000");
  SetDefaultIfMissing(info_, SQL_DROP_ASSERTION, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_DROP_CHARACTER_SET, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_DROP_COLLATION, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_DROP_DOMAIN, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_DROP_SCHEMA, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_DROP_TABLE, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_DROP_TRANSLATION, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_DROP_VIEW, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_EXPRESSIONS_IN_ORDERBY, "N");
  SetDefaultIfMissing(
      info_, SQL_GETDATA_EXTENSIONS,
      static_cast<uint32_t>(SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER));
  SetDefaultIfMissing(info_, SQL_GROUP_BY,
                      static_cast<uint16_t>(SQL_GB_GROUP_BY_CONTAINS_SELECT));
  SetDefaultIfMissing(info_, SQL_IDENTIFIER_CASE,
                      static_cast<uint16_t>(SQL_IC_MIXED));
  SetDefaultIfMissing(info_, SQL_IDENTIFIER_QUOTE_CHAR, "\"");
  SetDefaultIfMissing(info_, SQL_INDEX_KEYWORDS,
                      static_cast<uint32_t>(SQL_IK_NONE));
  SetDefaultIfMissing(
      info_, SQL_INFO_SCHEMA_VIEWS,
      static_cast<uint32_t>(SQL_ISV_TABLES | SQL_ISV_COLUMNS | SQL_ISV_VIEWS));
  SetDefaultIfMissing(info_, SQL_INSERT_STATEMENT,
                      static_cast<uint32_t>(SQL_IS_INSERT_LITERALS |
                                            SQL_IS_INSERT_SEARCHED |
                                            SQL_IS_SELECT_INTO));
  SetDefaultIfMissing(info_, SQL_INTEGRITY, "N");
  SetDefaultIfMissing(info_, SQL_KEYWORDS, "");
  SetDefaultIfMissing(info_, SQL_LIKE_ESCAPE_CLAUSE, "Y");
  SetDefaultIfMissing(info_, SQL_MAX_ASYNC_CONCURRENT_STATEMENTS,
                      static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_BINARY_LITERAL_LEN,
                      static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_CATALOG_NAME_LEN,
                      static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_CHAR_LITERAL_LEN,
                      static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_COLUMN_NAME_LEN, static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_COLUMNS_IN_GROUP_BY,
                      static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_COLUMNS_IN_INDEX,
                      static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_COLUMNS_IN_ORDER_BY,
                      static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_COLUMNS_IN_SELECT,
                      static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_COLUMNS_IN_TABLE,
                      static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_CURSOR_NAME_LEN, static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_DRIVER_CONNECTIONS,
                      static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_IDENTIFIER_LEN,
                      static_cast<uint16_t>(65535));
  SetDefaultIfMissing(info_, SQL_MAX_INDEX_SIZE, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_PROCEDURE_NAME_LEN,
                      static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_ROW_SIZE, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_ROW_SIZE_INCLUDES_LONG, "N");
  SetDefaultIfMissing(info_, SQL_MAX_SCHEMA_NAME_LEN, static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_STATEMENT_LEN, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_TABLE_NAME_LEN, static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_TABLES_IN_SELECT,
                      static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_MAX_USER_NAME_LEN, static_cast<uint16_t>(0));
  SetDefaultIfMissing(info_, SQL_NON_NULLABLE_COLUMNS,
                      static_cast<uint16_t>(SQL_NNC_NULL));
  SetDefaultIfMissing(info_, SQL_NULL_COLLATION,
                      static_cast<uint16_t>(SQL_NC_END));
  SetDefaultIfMissing(info_, SQL_NUMERIC_FUNCTIONS, static_cast<uint32_t>(0));
  SetDefaultIfMissing(
      info_, SQL_OJ_CAPABILITIES,
      static_cast<uint32_t>(SQL_OJ_LEFT | SQL_OJ_RIGHT | SQL_OJ_FULL));
  SetDefaultIfMissing(info_, SQL_ORDER_BY_COLUMNS_IN_SELECT, "Y");
  SetDefaultIfMissing(info_, SQL_PROCEDURE_TERM, "");
  SetDefaultIfMissing(info_, SQL_PROCEDURES, "N");
  SetDefaultIfMissing(info_, SQL_QUOTED_IDENTIFIER_CASE,
                      static_cast<uint16_t>(SQL_IC_SENSITIVE));
  SetDefaultIfMissing(info_, SQL_SCHEMA_TERM, "schema");
  SetDefaultIfMissing(info_, SQL_SCHEMA_USAGE,
                      static_cast<uint32_t>(SQL_SU_DML_STATEMENTS));
  SetDefaultIfMissing(info_, SQL_SEARCH_PATTERN_ESCAPE, "\\");
  SetDefaultIfMissing(info_, SQL_SERVER_NAME,
                      "Hardcharger"); // This might actually need to be the hostname.
  SetDefaultIfMissing(info_, SQL_SQL_CONFORMANCE,
                      static_cast<uint32_t>(SQL_SC_SQL92_ENTRY));
  SetDefaultIfMissing(info_, SQL_SQL92_DATETIME_FUNCTIONS,
                      static_cast<uint32_t>(SQL_SDF_CURRENT_DATE |
                                            SQL_SDF_CURRENT_TIME |
                                            SQL_SDF_CURRENT_TIMESTAMP));
  SetDefaultIfMissing(info_, SQL_SQL92_FOREIGN_KEY_DELETE_RULE,
                      static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_SQL92_FOREIGN_KEY_UPDATE_RULE,
                      static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_SQL92_GRANT, static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_SQL92_NUMERIC_VALUE_FUNCTIONS,
                      static_cast<uint32_t>(0));
  SetDefaultIfMissing(info_, SQL_SQL92_PREDICATES,
                      static_cast<uint32_t>(SQL_SP_BETWEEN | SQL_SP_COMPARISON |
                                            SQL_SP_EXISTS | SQL_SP_IN |
                                            SQL_SP_ISNOTNULL | SQL_SP_ISNULL |
                                            SQL_SP_LIKE));
  SetDefaultIfMissing(info_, SQL_SQL92_RELATIONAL_JOIN_OPERATORS,
                      static_cast<uint32_t>(
                          SQL_SRJO_INNER_JOIN | SQL_SRJO_CROSS_JOIN |
                          SQL_SRJO_LEFT_OUTER_JOIN | SQL_SRJO_FULL_OUTER_JOIN |
                          SQL_SRJO_RIGHT_OUTER_JOIN));
  SetDefaultIfMissing(info_, SQL_SQL92_REVOKE, static_cast<uint32_t>(0));
  SetDefaultIfMissing(
      info_, SQL_SQL92_ROW_VALUE_CONSTRUCTOR,
      static_cast<uint32_t>(SQL_SRVC_VALUE_EXPRESSION | SQL_SRVC_NULL));
  SetDefaultIfMissing(
      info_, SQL_SQL92_STRING_FUNCTIONS,
      static_cast<uint32_t>(SQL_SSF_CONVERT | SQL_SSF_LOWER | SQL_SSF_UPPER |
                            SQL_SSF_SUBSTRING | SQL_SSF_TRIM_BOTH |
                            SQL_SSF_TRIM_LEADING | SQL_SSF_TRIM_TRAILING));
  SetDefaultIfMissing(info_, SQL_SQL92_VALUE_EXPRESSIONS,
                      static_cast<uint32_t>(SQL_SVE_CASE | SQL_SVE_CAST |
                                            SQL_SVE_COALESCE | SQL_SVE_NULLIF));
  SetDefaultIfMissing(info_, SQL_STANDARD_CLI_CONFORMANCE,
                      static_cast<uint32_t>(0));
  SetDefaultIfMissing(
      info_, SQL_STRING_FUNCTIONS,
      static_cast<uint32_t>(SQL_FN_STR_CONCAT | SQL_FN_STR_LCASE |
                            SQL_FN_STR_LENGTH | SQL_FN_STR_LTRIM |
                            SQL_FN_STR_RTRIM | SQL_FN_STR_SPACE |
                            SQL_FN_STR_SUBSTRING | SQL_FN_STR_UCASE));
  SetDefaultIfMissing(info_, SQL_SUBQUERIES,
                      static_cast<uint32_t>(SQL_SQ_CORRELATED_SUBQUERIES |
                                            SQL_SQ_COMPARISON | SQL_SQ_EXISTS |
                                            SQL_SQ_IN | SQL_SQ_QUANTIFIED));
  SetDefaultIfMissing(
      info_, SQL_SYSTEM_FUNCTIONS,
      static_cast<uint32_t>(SQL_FN_SYS_IFNULL | SQL_FN_SYS_USERNAME));
  SetDefaultIfMissing(info_, SQL_TIMEDATE_ADD_INTERVALS,
                      static_cast<uint32_t>(
                          SQL_FN_TSI_FRAC_SECOND | SQL_FN_TSI_SECOND |
                          SQL_FN_TSI_MINUTE | SQL_FN_TSI_HOUR | SQL_FN_TSI_DAY |
                          SQL_FN_TSI_WEEK | SQL_FN_TSI_MONTH |
                          SQL_FN_TSI_QUARTER | SQL_FN_TSI_YEAR));
  SetDefaultIfMissing(info_, SQL_TIMEDATE_DIFF_INTERVALS,
                      static_cast<uint32_t>(
                          SQL_FN_TSI_FRAC_SECOND | SQL_FN_TSI_SECOND |
                          SQL_FN_TSI_MINUTE | SQL_FN_TSI_HOUR | SQL_FN_TSI_DAY |
                          SQL_FN_TSI_WEEK | SQL_FN_TSI_MONTH |
                          SQL_FN_TSI_QUARTER | SQL_FN_TSI_YEAR));
  SetDefaultIfMissing(info_, SQL_UNION,
                      static_cast<uint32_t>(SQL_U_UNION | SQL_U_UNION_ALL));
  SetDefaultIfMissing(info_, SQL_XOPEN_CLI_YEAR, "1995");
  SetDefaultIfMissing(info_, SQL_ODBC_SQL_CONFORMANCE,
                      static_cast<uint16_t>(SQL_OSC_MINIMUM));
  SetDefaultIfMissing(info_, SQL_ODBC_SAG_CLI_CONFORMANCE,
                      static_cast<uint16_t>(SQL_OSCC_COMPLIANT));
  }

} // namespace hardcharger
} // namespace driver
