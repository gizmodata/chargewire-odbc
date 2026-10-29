/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include <odbcabstraction/odbc_impl/ParameterArray.h>

#include <odbcabstraction/encoding.h>
#include <odbcabstraction/exceptions.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace ODBC {

using driver::odbcabstraction::ParameterBinding;
using driver::odbcabstraction::ParameterRowException;
using driver::odbcabstraction::ParameterSet;

size_t FixedCTypeSize(SQLSMALLINT c_type) {
  switch (c_type) {
  case SQL_C_BIT:
  case SQL_C_TINYINT:
  case SQL_C_STINYINT:
  case SQL_C_UTINYINT:
    return 1;
  case SQL_C_SHORT:
  case SQL_C_SSHORT:
  case SQL_C_USHORT:
    return sizeof(SQLSMALLINT);
  case SQL_C_LONG:
  case SQL_C_SLONG:
  case SQL_C_ULONG:
    return sizeof(SQLINTEGER);
  case SQL_C_FLOAT:
    return sizeof(SQLREAL);
  case SQL_C_DOUBLE:
    return sizeof(SQLDOUBLE);
  case SQL_C_SBIGINT:
  case SQL_C_UBIGINT:
    return sizeof(SQLBIGINT);
  case SQL_C_TYPE_DATE:
  case SQL_C_DATE:
    return sizeof(SQL_DATE_STRUCT);
  case SQL_C_TYPE_TIME:
  case SQL_C_TIME:
    return sizeof(SQL_TIME_STRUCT);
  case SQL_C_TYPE_TIMESTAMP:
  case SQL_C_TIMESTAMP:
    return sizeof(SQL_TIMESTAMP_STRUCT);
  case SQL_C_NUMERIC:
    return sizeof(SQL_NUMERIC_STRUCT);
  case SQL_C_GUID:
    return sizeof(SQLGUID);
  default:
    return 0;
  }
}

bool IsVariableLengthCType(SQLSMALLINT c_type) {
  return c_type == SQL_C_CHAR || c_type == SQL_C_WCHAR || c_type == SQL_C_BINARY;
}

namespace {

std::string Where(size_t row, size_t column) {
  return "Parameter set " + std::to_string(row + 1) + ", parameter " + std::to_string(column + 1) + ": ";
}

// base + offset + row * stride, refusing an address that wraps around.
void* Advance(void* base, SQLULEN offset, SQLULEN row, SQLULEN stride, size_t column) {
  constexpr uintptr_t kMax = std::numeric_limits<uintptr_t>::max();
  if (stride != 0 && row > (kMax - offset) / stride) {
    throw ParameterRowException(row, Where(row, column) + "parameter array address overflows", "HY090");
  }
  const uintptr_t delta = static_cast<uintptr_t>(offset) + static_cast<uintptr_t>(row) * stride;
  const uintptr_t address = reinterpret_cast<uintptr_t>(base);
  if (address > kMax - delta) {
    throw ParameterRowException(row, Where(row, column) + "parameter array address overflows", "HY090");
  }
  return reinterpret_cast<void*>(address + delta);
}

ssize_t BoundedCharLength(const void* data, SQLLEN buffer_length) {
  if (buffer_length > 0) {
    const void* end = std::memchr(data, 0, static_cast<size_t>(buffer_length));
    return end ? static_cast<const char*>(end) - static_cast<const char*>(data) : buffer_length;
  }
  return static_cast<ssize_t>(std::strlen(static_cast<const char*>(data)));
}

ssize_t BoundedWideLength(const void* data, SQLLEN buffer_length) {
  const size_t unit = driver::odbcabstraction::GetSqlWCharSize();
  if (buffer_length <= 0) {
    return static_cast<ssize_t>(driver::odbcabstraction::wcsstrlen(data) * unit);
  }
  const size_t units = static_cast<size_t>(buffer_length) / unit;
  const auto* bytes = static_cast<const unsigned char*>(data);
  static const unsigned char zero[4] = {0, 0, 0, 0};
  for (size_t i = 0; i < units; ++i) {
    if (std::memcmp(bytes + i * unit, zero, unit) == 0) return static_cast<ssize_t>(i * unit);
  }
  return static_cast<ssize_t>(units * unit);
}

} // namespace

ParameterSet ResolveParameterArray(const std::vector<ParameterArrayColumn>& columns,
                                   const ParameterArrayLayout& layout) {
  if (layout.row_count == 0) {
    throw driver::odbcabstraction::DriverException("SQL_ATTR_PARAMSET_SIZE must be at least 1", "HY024");
  }
  const bool row_wise = layout.bind_type != SQL_PARAM_BIND_BY_COLUMN;
  const SQLULEN offset = layout.bind_offset ? *layout.bind_offset : 0;
  const size_t wchar_size = driver::odbcabstraction::GetSqlWCharSize();

  ParameterSet set;
  set.column_count = columns.size();
  for (SQLULEN row = 0; row < layout.row_count; ++row) {
    if (layout.operations && layout.operations[row] == SQL_PARAM_IGNORE) continue;
    for (size_t column = 0; column < columns.size(); ++column) {
      const ParameterArrayColumn& bound = columns[column];
      const bool variable = IsVariableLengthCType(bound.c_type);
      const size_t fixed_size = FixedCTypeSize(bound.c_type);
      // A C type the driver cannot convert (SQL_C_DEFAULT, intervals, ...) can
      // still carry NULL, as pyodbc binds None; any other value is refused below.
      const bool convertible = variable || fixed_size != 0;

      SQLULEN data_stride = 0;
      SQLULEN indicator_stride = 0;
      if (row_wise) {
        data_stride = indicator_stride = layout.bind_type;
      } else {
        indicator_stride = sizeof(SQLLEN);
        if (variable) {
          if (layout.row_count > 1 && bound.buffer_length <= 0) {
            throw ParameterRowException(row, Where(row, column) +
                "a column-wise array of character or binary parameters needs a positive BufferLength", "HY090");
          }
          data_stride = bound.buffer_length > 0 ? static_cast<SQLULEN>(bound.buffer_length) : 0;
        } else {
          data_stride = fixed_size;
        }
      }

      // ODBC adds the bind offset to bound addresses only; a null pointer stays null.
      void* data = bound.data ? Advance(bound.data, offset, row, data_stride, column) : nullptr;
      void* indicator = bound.indicator ? Advance(bound.indicator, offset, row, indicator_stride, column) : nullptr;

      ParameterBinding binding;
      binding.data_ptr = data;
      binding.indicator_ptr = static_cast<ssize_t*>(indicator);
      binding.c_type = bound.c_type;
      binding.sql_type = bound.sql_type;
      binding.column_size = bound.column_size;
      binding.decimal_digits = bound.decimal_digits;
      binding.buffer_length = bound.buffer_length;

      SQLLEN indicator_value = 0;
      if (indicator) std::memcpy(&indicator_value, indicator, sizeof(indicator_value));

      if ((indicator && indicator_value == SQL_NULL_DATA) || (!data && !indicator)) {
        // A null data pointer without an indicator is the NULL value the binding was made for.
        binding.is_null = true;
      } else if (indicator && (indicator_value == SQL_DATA_AT_EXEC ||
                               indicator_value <= SQL_LEN_DATA_AT_EXEC_OFFSET)) {
        throw ParameterRowException(row, Where(row, column) +
            "data-at-execution parameters (SQLParamData/SQLPutData) are not supported", "HYC00");
      } else if (indicator && indicator_value == SQL_DEFAULT_PARAM) {
        throw ParameterRowException(row, Where(row, column) + "SQL_DEFAULT_PARAM is not supported", "HYC00");
      } else if (indicator && indicator_value == SQL_COLUMN_IGNORE) {
        throw ParameterRowException(row, Where(row, column) + "SQL_COLUMN_IGNORE is not supported", "HYC00");
      } else if (!convertible) {
        throw ParameterRowException(row, Where(row, column) + "unsupported C type " + std::to_string(bound.c_type), "HY003");
      } else if (variable) {
        SQLLEN length = indicator ? indicator_value
                                  : (bound.c_type == SQL_C_BINARY ? bound.buffer_length : SQL_NTS);
        if (length == SQL_NTS) {
          if (bound.c_type == SQL_C_BINARY) {
            throw ParameterRowException(row, Where(row, column) + "binary parameters need an explicit length", "HY090");
          }
          if (!data) {
            throw ParameterRowException(row, Where(row, column) + "null-terminated value has a null data pointer", "HY009");
          }
          length = bound.c_type == SQL_C_CHAR ? BoundedCharLength(data, bound.buffer_length)
                                              : BoundedWideLength(data, bound.buffer_length);
        } else if (length < 0) {
          throw ParameterRowException(row, Where(row, column) + "invalid length or indicator " + std::to_string(length), "HY090");
        }
        if (!row_wise && layout.row_count > 1 && length > bound.buffer_length) {
          throw ParameterRowException(row, Where(row, column) + "length " + std::to_string(length) +
              " exceeds the array element's BufferLength " + std::to_string(bound.buffer_length), "HY090");
        }
        if (bound.c_type == SQL_C_WCHAR && length % static_cast<SQLLEN>(wchar_size) != 0) {
          throw ParameterRowException(row, Where(row, column) + "wide-character length " + std::to_string(length) +
              " is not a whole number of characters", "HY090");
        }
        if (length > std::numeric_limits<int32_t>::max()) {
          throw ParameterRowException(row, Where(row, column) + "value is too long", "HY090");
        }
        if (length > 0 && !data) {
          throw ParameterRowException(row, Where(row, column) + "non-NULL value has a null data pointer", "HY009");
        }
        binding.length = static_cast<ssize_t>(length);
      } else if (!data) {
        throw ParameterRowException(row, Where(row, column) + "non-NULL value has a null data pointer", "HY009");
      }
      set.cells.push_back(binding);
    }
    set.source_rows.push_back(static_cast<size_t>(row));
    ++set.row_count;
  }
  return set;
}

} // namespace ODBC
