/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "primitive_array_accessor.h"

namespace driver {
namespace chargewire_odbc {

using namespace arrow;
using namespace odbcabstraction;

template <typename ARROW_ARRAY, CDataType TARGET_TYPE>
PrimitiveArrayChargewireAccessor<
    ARROW_ARRAY, TARGET_TYPE>::PrimitiveArrayChargewireAccessor(Array *array)
    : ChargewireAccessor<
          ARROW_ARRAY, TARGET_TYPE,
          PrimitiveArrayChargewireAccessor<ARROW_ARRAY, TARGET_TYPE>>(array) {}

template <typename ARROW_ARRAY, CDataType TARGET_TYPE>
size_t
PrimitiveArrayChargewireAccessor<ARROW_ARRAY, TARGET_TYPE>::GetColumnarData_impl(
    ColumnBinding *binding, int64_t starting_row,
    int64_t cells, int64_t &value_offset, bool update_value_offset,
    odbcabstraction::Diagnostics &diagnostics, uint16_t* row_status_array) {
  return CopyFromArrayValuesToBinding<ARROW_ARRAY>(this->GetArray(), binding, starting_row, cells);
}

template <typename ARROW_ARRAY, CDataType TARGET_TYPE>
size_t PrimitiveArrayChargewireAccessor<ARROW_ARRAY, TARGET_TYPE>::GetCellLength_impl(ColumnBinding *binding) const {
  return sizeof(typename ARROW_ARRAY::TypeClass::c_type);
}

template class PrimitiveArrayChargewireAccessor<
    Int64Array, odbcabstraction::CDataType_SBIGINT>;
template class PrimitiveArrayChargewireAccessor<
    Int32Array, odbcabstraction::CDataType_SLONG>;
template class PrimitiveArrayChargewireAccessor<
    Int16Array, odbcabstraction::CDataType_SSHORT>;
template class PrimitiveArrayChargewireAccessor<
    Int8Array, odbcabstraction::CDataType_STINYINT>;
template class PrimitiveArrayChargewireAccessor<
    UInt64Array, odbcabstraction::CDataType_UBIGINT>;
template class PrimitiveArrayChargewireAccessor<
    UInt32Array, odbcabstraction::CDataType_ULONG>;
template class PrimitiveArrayChargewireAccessor<
    UInt16Array, odbcabstraction::CDataType_USHORT>;
template class PrimitiveArrayChargewireAccessor<
    UInt8Array, odbcabstraction::CDataType_UTINYINT>;
template class PrimitiveArrayChargewireAccessor<
    DoubleArray, odbcabstraction::CDataType_DOUBLE>;
template class PrimitiveArrayChargewireAccessor<
    FloatArray, odbcabstraction::CDataType_FLOAT>;

} // namespace chargewire
} // namespace driver
