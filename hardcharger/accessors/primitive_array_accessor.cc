/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "primitive_array_accessor.h"

namespace driver {
namespace hardcharger {

using namespace arrow;
using namespace odbcabstraction;

template <typename ARROW_ARRAY, CDataType TARGET_TYPE>
PrimitiveArrayHardchargerAccessor<
    ARROW_ARRAY, TARGET_TYPE>::PrimitiveArrayHardchargerAccessor(Array *array)
    : HardchargerAccessor<
          ARROW_ARRAY, TARGET_TYPE,
          PrimitiveArrayHardchargerAccessor<ARROW_ARRAY, TARGET_TYPE>>(array) {}

template <typename ARROW_ARRAY, CDataType TARGET_TYPE>
size_t
PrimitiveArrayHardchargerAccessor<ARROW_ARRAY, TARGET_TYPE>::GetColumnarData_impl(
    ColumnBinding *binding, int64_t starting_row,
    int64_t cells, int64_t &value_offset, bool update_value_offset,
    odbcabstraction::Diagnostics &diagnostics, uint16_t* row_status_array) {
  return CopyFromArrayValuesToBinding<ARROW_ARRAY>(this->GetArray(), binding, starting_row, cells);
}

template <typename ARROW_ARRAY, CDataType TARGET_TYPE>
size_t PrimitiveArrayHardchargerAccessor<ARROW_ARRAY, TARGET_TYPE>::GetCellLength_impl(ColumnBinding *binding) const {
  return sizeof(typename ARROW_ARRAY::TypeClass::c_type);
}

template class PrimitiveArrayHardchargerAccessor<
    Int64Array, odbcabstraction::CDataType_SBIGINT>;
template class PrimitiveArrayHardchargerAccessor<
    Int32Array, odbcabstraction::CDataType_SLONG>;
template class PrimitiveArrayHardchargerAccessor<
    Int16Array, odbcabstraction::CDataType_SSHORT>;
template class PrimitiveArrayHardchargerAccessor<
    Int8Array, odbcabstraction::CDataType_STINYINT>;
template class PrimitiveArrayHardchargerAccessor<
    UInt64Array, odbcabstraction::CDataType_UBIGINT>;
template class PrimitiveArrayHardchargerAccessor<
    UInt32Array, odbcabstraction::CDataType_ULONG>;
template class PrimitiveArrayHardchargerAccessor<
    UInt16Array, odbcabstraction::CDataType_USHORT>;
template class PrimitiveArrayHardchargerAccessor<
    UInt8Array, odbcabstraction::CDataType_UTINYINT>;
template class PrimitiveArrayHardchargerAccessor<
    DoubleArray, odbcabstraction::CDataType_DOUBLE>;
template class PrimitiveArrayHardchargerAccessor<
    FloatArray, odbcabstraction::CDataType_FLOAT>;

} // namespace hardcharger
} // namespace driver
