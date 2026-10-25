/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "primitive_array_accessor.h"

namespace driver {
namespace warpsql_odbc {

using namespace arrow;
using namespace odbcabstraction;

template <typename ARROW_ARRAY, CDataType TARGET_TYPE>
PrimitiveArrayWarpSqlAccessor<
    ARROW_ARRAY, TARGET_TYPE>::PrimitiveArrayWarpSqlAccessor(Array *array)
    : WarpSqlAccessor<
          ARROW_ARRAY, TARGET_TYPE,
          PrimitiveArrayWarpSqlAccessor<ARROW_ARRAY, TARGET_TYPE>>(array) {}

template <typename ARROW_ARRAY, CDataType TARGET_TYPE>
size_t
PrimitiveArrayWarpSqlAccessor<ARROW_ARRAY, TARGET_TYPE>::GetColumnarData_impl(
    ColumnBinding *binding, int64_t starting_row,
    int64_t cells, int64_t &value_offset, bool update_value_offset,
    odbcabstraction::Diagnostics &diagnostics, uint16_t* row_status_array) {
  return CopyFromArrayValuesToBinding<ARROW_ARRAY>(this->GetArray(), binding, starting_row, cells);
}

template <typename ARROW_ARRAY, CDataType TARGET_TYPE>
size_t PrimitiveArrayWarpSqlAccessor<ARROW_ARRAY, TARGET_TYPE>::GetCellLength_impl(ColumnBinding *binding) const {
  return sizeof(typename ARROW_ARRAY::TypeClass::c_type);
}

template class PrimitiveArrayWarpSqlAccessor<
    Int64Array, odbcabstraction::CDataType_SBIGINT>;
template class PrimitiveArrayWarpSqlAccessor<
    Int32Array, odbcabstraction::CDataType_SLONG>;
template class PrimitiveArrayWarpSqlAccessor<
    Int16Array, odbcabstraction::CDataType_SSHORT>;
template class PrimitiveArrayWarpSqlAccessor<
    Int8Array, odbcabstraction::CDataType_STINYINT>;
template class PrimitiveArrayWarpSqlAccessor<
    UInt64Array, odbcabstraction::CDataType_UBIGINT>;
template class PrimitiveArrayWarpSqlAccessor<
    UInt32Array, odbcabstraction::CDataType_ULONG>;
template class PrimitiveArrayWarpSqlAccessor<
    UInt16Array, odbcabstraction::CDataType_USHORT>;
template class PrimitiveArrayWarpSqlAccessor<
    UInt8Array, odbcabstraction::CDataType_UTINYINT>;
template class PrimitiveArrayWarpSqlAccessor<
    DoubleArray, odbcabstraction::CDataType_DOUBLE>;
template class PrimitiveArrayWarpSqlAccessor<
    FloatArray, odbcabstraction::CDataType_FLOAT>;

} // namespace warpsql
} // namespace driver
