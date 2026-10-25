/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "arrow/testing/builder.h"
#include "primitive_array_accessor.h"
#include <odbcabstraction/diagnostics.h>
#include "gtest/gtest.h"

namespace driver {
namespace warpsql_odbc {

using namespace arrow;
using namespace odbcabstraction;

template <typename ARROW_ARRAY, CDataType TARGET_TYPE>
void TestPrimitiveArraySqlAccessor() {
  typedef typename ARROW_ARRAY::TypeClass::c_type c_type;

  std::vector<c_type> values = {0, 1, 2, 3, 127};

  std::shared_ptr<Array> array;
  ArrayFromVector<typename ARROW_ARRAY::TypeClass>(values, &array);

  PrimitiveArrayWarpSqlAccessor<ARROW_ARRAY, TARGET_TYPE> accessor(
      array.get());

  std::vector<c_type> buffer(values.size());
  std::vector<ssize_t> strlen_buffer(values.size());

  ColumnBinding binding(TARGET_TYPE, 0, 0, buffer.data(), values.size(),
                        strlen_buffer.data());

  int64_t value_offset = 0;
  driver::odbcabstraction::Diagnostics diagnostics("Dummy", "Dummy", odbcabstraction::V_3);
  ASSERT_EQ(values.size(),
            accessor.GetColumnarData(&binding, 0, values.size(), value_offset, false, diagnostics, nullptr));

  for (int i = 0; i < values.size(); ++i) {
    ASSERT_EQ(sizeof(c_type), strlen_buffer[i]);
    ASSERT_EQ(values[i], buffer[i]);
  }
}

TEST(PrimitiveArrayWarpSqlAccessor, Test_Int64Array_CDataType_SBIGINT) {
  TestPrimitiveArraySqlAccessor<Int64Array, CDataType_SBIGINT>();
}

TEST(PrimitiveArrayWarpSqlAccessor, Test_Int32Array_CDataType_SLONG) {
  TestPrimitiveArraySqlAccessor<Int32Array, CDataType_SLONG>();
}

TEST(PrimitiveArrayWarpSqlAccessor, Test_Int16Array_CDataType_SSHORT) {
  TestPrimitiveArraySqlAccessor<Int16Array, CDataType_SSHORT>();
}

TEST(PrimitiveArrayWarpSqlAccessor, Test_Int8Array_CDataType_STINYINT) {
  TestPrimitiveArraySqlAccessor<Int8Array, CDataType_STINYINT>();
}

TEST(PrimitiveArrayWarpSqlAccessor, Test_UInt64Array_CDataType_UBIGINT) {
  TestPrimitiveArraySqlAccessor<UInt64Array, CDataType_UBIGINT>();
}

TEST(PrimitiveArrayWarpSqlAccessor, Test_UInt32Array_CDataType_ULONG) {
  TestPrimitiveArraySqlAccessor<UInt32Array, CDataType_ULONG>();
}

TEST(PrimitiveArrayWarpSqlAccessor, Test_UInt16Array_CDataType_USHORT) {
  TestPrimitiveArraySqlAccessor<UInt16Array, CDataType_USHORT>();
}

TEST(PrimitiveArrayWarpSqlAccessor, Test_UInt8Array_CDataType_UTINYINT) {
  TestPrimitiveArraySqlAccessor<UInt8Array, CDataType_UTINYINT>();
}

TEST(PrimitiveArrayWarpSqlAccessor, Test_FloatArray_CDataType_FLOAT) {
  TestPrimitiveArraySqlAccessor<FloatArray, CDataType_FLOAT>();
}

TEST(PrimitiveArrayWarpSqlAccessor, Test_DoubleArray_CDataType_DOUBLE) {
  TestPrimitiveArraySqlAccessor<DoubleArray, CDataType_DOUBLE>();
}

} // namespace warpsql
} // namespace driver
