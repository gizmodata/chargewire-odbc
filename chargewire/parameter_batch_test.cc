/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

/*
 * Parameter arrays: resolving each parameter set's buffers (column-wise,
 * row-wise, bind offsets, indicators) and converting them to the Arrow
 * parameter batch, one row per set. Regression for pyodbc fast_executemany,
 * which binds row-wise at offset-relative addresses and crashed the driver.
 */

#include "gtest/gtest.h"

#include <arrow/array.h>
#include <arrow/util/decimal.h>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include <odbcabstraction/platform.h>
#include <sql.h>
#include <sqlext.h>

#include "odbcabstraction/exceptions.h"
#include "odbcabstraction/odbc_impl/ParameterArray.h"
#include "parameter_batch.h"

namespace driver {
namespace chargewire_odbc {
namespace {

using odbcabstraction::ParameterRowException;
using ODBC::ParameterArrayColumn;
using ODBC::ParameterArrayLayout;
using ODBC::ResolveParameterArray;

ParameterArrayColumn Column(SQLSMALLINT c_type, void* data, SQLLEN* indicator, SQLLEN buffer_length = 0) {
  ParameterArrayColumn column;
  column.c_type = c_type;
  column.data = data;
  column.indicator = indicator;
  column.buffer_length = buffer_length;
  return column;
}

template <typename F>
std::string RefusalState(F&& f, size_t* row = nullptr) {
  try {
    f();
  } catch (const ParameterRowException& e) {
    if (row) *row = e.GetRow();
    return e.GetSqlState();
  } catch (const odbcabstraction::DriverException& e) {
    return "not a row error: " + e.GetSqlState();
  }
  return "accepted";
}

TEST(ParameterArray, ColumnWiseStridesByElementSize) {
  SQLINTEGER ids[3] = {7, 8, 9};
  char names[3][6] = {{'a', 'b', 'X', 'X', 'X', 'X'}, "cde", "zzzz"};
  SQLLEN name_ind[3] = {2, SQL_NTS, SQL_NULL_DATA};
  ParameterArrayLayout layout;
  layout.row_count = 3;
  auto set = ResolveParameterArray({Column(SQL_C_SLONG, ids, nullptr), Column(SQL_C_CHAR, names, name_ind, 6)}, layout);
  ASSERT_EQ(set.row_count, 3u);
  for (int r = 0; r < 3; ++r) EXPECT_EQ(set.at(r, 0).data_ptr, &ids[r]);
  EXPECT_EQ(set.at(0, 1).length, 2);   // explicit length, no terminator
  EXPECT_EQ(set.at(1, 1).length, 3);   // SQL_NTS
  EXPECT_TRUE(set.at(2, 1).is_null);
  EXPECT_EQ(set.at(1, 1).data_ptr, names[1]);
}

// pyodbc's fast_executemany layout: rows of [value][SQLLEN indicator] bound at
// addresses relative to 16, with SQL_ATTR_PARAM_BIND_OFFSET_PTR = buffer - 16.
TEST(ParameterArray, RowWiseAddressesAreRelativeToTheBindOffset) {
  struct Row { SQLBIGINT value; SQLLEN value_ind; SQLWCHAR text[4]; SQLLEN text_ind; };
  Row rows[2] = {{11, 8, {'h', 'i', 0, 0}, 4}, {-22, SQL_NULL_DATA, {'x', 0, 0, 0}, SQL_NTS}};
  char* base = reinterpret_cast<char*>(16);
  ParameterArrayLayout layout;
  layout.row_count = 2;
  layout.bind_type = sizeof(Row);
  const SQLULEN offset = reinterpret_cast<SQLULEN>(rows) - 16;
  layout.bind_offset = &offset;
  auto set = ResolveParameterArray(
      {Column(SQL_C_SBIGINT, base + offsetof(Row, value), reinterpret_cast<SQLLEN*>(base + offsetof(Row, value_ind))),
       Column(SQL_C_WCHAR, base + offsetof(Row, text), reinterpret_cast<SQLLEN*>(base + offsetof(Row, text_ind)),
              sizeof(rows[0].text))},
      layout);
  EXPECT_EQ(set.at(0, 0).data_ptr, &rows[0].value);
  EXPECT_FALSE(set.at(0, 0).is_null);
  EXPECT_TRUE(set.at(1, 0).is_null);
  EXPECT_EQ(set.at(1, 1).data_ptr, rows[1].text);
  EXPECT_EQ(set.at(0, 1).length, 4);
  EXPECT_EQ(set.at(1, 1).length, static_cast<ssize_t>(sizeof(SQLWCHAR)));
}

TEST(ParameterArray, IgnoredSetsAreLeftOut) {
  SQLINTEGER ids[3] = {1, 2, 3};
  SQLUSMALLINT ops[3] = {SQL_PARAM_IGNORE, SQL_PARAM_PROCEED, SQL_PARAM_PROCEED};
  ParameterArrayLayout layout;
  layout.row_count = 3;
  layout.operations = ops;
  auto set = ResolveParameterArray({Column(SQL_C_SLONG, ids, nullptr)}, layout);
  ASSERT_EQ(set.row_count, 2u);
  EXPECT_EQ(set.source_rows, (std::vector<size_t>{1, 2}));
  EXPECT_EQ(set.at(0, 0).data_ptr, &ids[1]);
}

TEST(ParameterArray, RefusesWhatItCannotSendNamingTheRow) {
  SQLINTEGER ids[2] = {1, 2};
  char text[2][4] = {"ab", "cd"};
  ParameterArrayLayout two;
  two.row_count = 2;
  size_t row = 99;

  SQLLEN dae[2] = {SQL_NTS, SQL_DATA_AT_EXEC};
  EXPECT_EQ(RefusalState([&] { ResolveParameterArray({Column(SQL_C_CHAR, text, dae, 4)}, two); }, &row), "HYC00");
  EXPECT_EQ(row, 1u);
  SQLLEN len_dae[2] = {SQL_LEN_DATA_AT_EXEC(3), 0};
  EXPECT_EQ(RefusalState([&] { ResolveParameterArray({Column(SQL_C_CHAR, text, len_dae, 4)}, two); }), "HYC00");
  SQLLEN negative[2] = {2, -7};
  EXPECT_EQ(RefusalState([&] { ResolveParameterArray({Column(SQL_C_CHAR, text, negative, 4)}, two); }), "HY090");
  SQLLEN too_long[2] = {5, 1};
  EXPECT_EQ(RefusalState([&] { ResolveParameterArray({Column(SQL_C_CHAR, text, too_long, 4)}, two); }), "HY090");
  SQLLEN nts[2] = {SQL_NTS, SQL_NTS};
  EXPECT_EQ(RefusalState([&] { ResolveParameterArray({Column(SQL_C_CHAR, text, nts, 0)}, two); }), "HY090");
  SQLLEN present[2] = {0, 0};
  EXPECT_EQ(RefusalState([&] { ResolveParameterArray({Column(SQL_C_SLONG, nullptr, present)}, two); }), "HY009");
  EXPECT_EQ(RefusalState([&] { ResolveParameterArray({Column(SQL_C_DEFAULT, ids, present)}, two); }), "HY003");
  SQLLEN default_param[2] = {SQL_DEFAULT_PARAM, 0};
  EXPECT_EQ(RefusalState([&] { ResolveParameterArray({Column(SQL_C_SLONG, ids, default_param)}, two); }), "HYC00");

  // The valid twins of the refusals above are accepted.
  SQLLEN ok_len[2] = {2, 2};
  EXPECT_EQ(RefusalState([&] { ResolveParameterArray({Column(SQL_C_CHAR, text, ok_len, 4)}, two); }), "accepted");
  SQLLEN nulls[2] = {SQL_NULL_DATA, SQL_NULL_DATA};
  EXPECT_EQ(RefusalState([&] { ResolveParameterArray({Column(SQL_C_DEFAULT, nullptr, nulls)}, two); }), "accepted");
  EXPECT_EQ(RefusalState([&] { ResolveParameterArray({Column(SQL_C_SLONG, ids, present)}, two); }), "accepted");
}

std::shared_ptr<arrow::RecordBatch> Batch(const std::shared_ptr<arrow::DataType>& type,
                                          const std::vector<ParameterArrayColumn>& columns, SQLULEN rows,
                                          SQLULEN bind_type = SQL_PARAM_BIND_BY_COLUMN) {
  ParameterArrayLayout layout;
  layout.row_count = rows;
  layout.bind_type = bind_type;
  return BuildParameterBatch(arrow::schema({arrow::field("p", type)}), ResolveParameterArray(columns, layout));
}

TEST(ParameterBatch, OneRowPerSetWithNulls) {
  SQLBIGINT values[4] = {1, -2, 3, 40000};
  SQLLEN ind[4] = {0, SQL_NULL_DATA, 0, 0};
  auto batch = Batch(arrow::int32(), {Column(SQL_C_SBIGINT, values, ind)}, 4);
  ASSERT_EQ(batch->num_rows(), 4);
  auto ints = std::static_pointer_cast<arrow::Int32Array>(batch->column(0));
  EXPECT_EQ(ints->Value(0), 1);
  EXPECT_TRUE(ints->IsNull(1));
  EXPECT_EQ(ints->Value(3), 40000);
}

TEST(ParameterBatch, NarrowingOutOfRangeIsRefusedForItsRow) {
  SQLBIGINT values[3] = {1, 70000, 3};
  size_t row = 99;
  EXPECT_EQ(RefusalState([&] { Batch(arrow::int16(), {Column(SQL_C_SBIGINT, values, nullptr)}, 3); }, &row), "22003");
  EXPECT_EQ(row, 1u);
}

TEST(ParameterBatch, NumericStructKeepsScaleAndSign) {
  SQL_NUMERIC_STRUCT numeric[2];
  std::memset(numeric, 0, sizeof(numeric));
  const unsigned long long magnitudes[2] = {1234567ULL, 50ULL};  // 123.4567 and -0.5 (scale 2: 0.50)
  const unsigned char scales[2] = {4, 2};
  for (int r = 0; r < 2; ++r) {
    numeric[r].precision = 18;
    numeric[r].scale = scales[r];
    numeric[r].sign = r == 0 ? 1 : 0;
    for (int i = 0; i < 8; ++i) numeric[r].val[i] = static_cast<SQLCHAR>(magnitudes[r] >> (8 * i));
  }
  auto batch = Batch(arrow::decimal128(18, 4), {Column(SQL_C_NUMERIC, numeric, nullptr)}, 2);
  auto decimals = std::static_pointer_cast<arrow::Decimal128Array>(batch->column(0));
  EXPECT_EQ(decimals->FormatValue(0), "123.4567");
  EXPECT_EQ(decimals->FormatValue(1), "-0.5000");
  numeric[0].scale = 5;  // 12.34567 cannot be stored at scale 4
  EXPECT_EQ(RefusalState([&] { Batch(arrow::decimal128(18, 4), {Column(SQL_C_NUMERIC, numeric, nullptr)}, 2); }), "22003");
}

TEST(ParameterBatch, TimestampsKeepMicrosecondsAndPre1970Dates) {
  SQL_TIMESTAMP_STRUCT ts[2] = {{2026, 10, 28, 13, 45, 30, 123456000}, {1901, 2, 3, 4, 5, 6, 789000}};
  auto batch = Batch(arrow::timestamp(arrow::TimeUnit::MICRO), {Column(SQL_C_TYPE_TIMESTAMP, ts, nullptr)}, 2);
  auto micros = std::static_pointer_cast<arrow::TimestampArray>(batch->column(0));
  EXPECT_EQ(micros->Value(0), 1793195130123456LL);
  EXPECT_EQ(micros->Value(1), -2174586894000000LL + 789LL);

  char text[2][32] = {"2026-10-28 13:45:30.5", "2026-10-28T13:45:30+02:00"};
  SQLLEN ind[2] = {SQL_NTS, SQL_NTS};
  auto parsed = Batch(arrow::timestamp(arrow::TimeUnit::MICRO), {Column(SQL_C_CHAR, text, ind, 32)}, 1);
  EXPECT_EQ(std::static_pointer_cast<arrow::TimestampArray>(parsed->column(0))->Value(0), 1793195130500000LL);
  // A UTC offset would be silently ignored, so it is refused.
  EXPECT_EQ(RefusalState([&] { Batch(arrow::timestamp(arrow::TimeUnit::MICRO), {Column(SQL_C_CHAR, text, ind, 32)}, 2); }),
            "22007");
}

TEST(ParameterBatch, GuidRendersAsText) {
  SQLGUID guid = {0x01234567, 0x89ab, 0xcdef, {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};
  auto batch = Batch(arrow::utf8(), {Column(SQL_C_GUID, &guid, nullptr)}, 1);
  EXPECT_EQ(std::static_pointer_cast<arrow::StringArray>(batch->column(0))->GetString(0),
            "01234567-89ab-cdef-0123-456789abcdef");
}

}  // namespace
}  // namespace chargewire_odbc
}  // namespace driver
