/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include <odbcabstraction/platform.h>
#include <odbcabstraction/types.h>
#include <sql.h>
#include <sqlext.h>
#include <vector>

namespace ODBC {

/// One bound parameter as the APD/IPD describe it (SQLBindParameter's arguments).
struct ParameterArrayColumn {
  SQLPOINTER data = nullptr;         // SQL_DESC_DATA_PTR
  SQLLEN* indicator = nullptr;       // SQL_DESC_INDICATOR_PTR / SQL_DESC_OCTET_LENGTH_PTR
  SQLSMALLINT c_type = 0;            // APD SQL_DESC_CONCISE_TYPE
  SQLSMALLINT sql_type = 0;          // IPD SQL_DESC_CONCISE_TYPE
  SQLULEN column_size = 0;           // IPD SQL_DESC_LENGTH
  SQLSMALLINT decimal_digits = 0;    // IPD SQL_DESC_SCALE
  SQLLEN buffer_length = 0;          // APD SQL_DESC_OCTET_LENGTH
};

/// The APD header fields that lay out a parameter array.
struct ParameterArrayLayout {
  SQLULEN row_count = 1;                       // SQL_ATTR_PARAMSET_SIZE
  SQLULEN bind_type = SQL_PARAM_BIND_BY_COLUMN; // SQL_ATTR_PARAM_BIND_TYPE
  const SQLULEN* bind_offset = nullptr;        // SQL_ATTR_PARAM_BIND_OFFSET_PTR
  const SQLUSMALLINT* operations = nullptr;    // SQL_ATTR_PARAM_OPERATION_PTR
};

/// Resolves every parameter value's address and length for each parameter set,
/// following the ODBC rules for column-wise and row-wise binding and bind
/// offsets. It reads the indicators and validates every value without
/// converting it. A value it cannot send (data-at-execution, a negative length,
/// a null data pointer for a non-NULL value, ...) throws ParameterRowException
/// naming the row, so no input makes the driver read through a bad address that
/// the indicator or layout would otherwise lead it to.
driver::odbcabstraction::ParameterSet ResolveParameterArray(
    const std::vector<ParameterArrayColumn>& columns, const ParameterArrayLayout& layout);

/// The size of one value of a fixed-length C type, or 0 for character and
/// binary types (whose element size is the bound BufferLength).
size_t FixedCTypeSize(SQLSMALLINT c_type);

/// True for C types whose value length comes from the indicator.
bool IsVariableLengthCType(SQLSMALLINT c_type);

} // namespace ODBC
