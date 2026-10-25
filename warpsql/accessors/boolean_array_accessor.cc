/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "boolean_array_accessor.h"

namespace driver {
namespace warpsql_odbc {

using namespace arrow;
using namespace odbcabstraction;

template <CDataType TARGET_TYPE>
BooleanArrayWarpSqlAccessor<TARGET_TYPE>::BooleanArrayWarpSqlAccessor(
    Array *array)
    : WarpSqlAccessor<BooleanArray, TARGET_TYPE,
                        BooleanArrayWarpSqlAccessor<TARGET_TYPE>>(array) {}

template <CDataType TARGET_TYPE>
RowStatus BooleanArrayWarpSqlAccessor<TARGET_TYPE>::MoveSingleCell_impl(
    ColumnBinding *binding, int64_t arrow_row, int64_t i, int64_t &value_offset,
    bool update_value_offset, odbcabstraction::Diagnostics &diagnostics) {
  typedef unsigned char c_type;
  bool value = this->GetArray()->Value(arrow_row);

  auto *buffer = static_cast<c_type *>(binding->buffer);
  buffer[i] = value ? 1 : 0;

  if (binding->strlen_buffer) {
    binding->strlen_buffer[i] = static_cast<ssize_t>(GetCellLength_impl(binding));
  }

  return odbcabstraction::RowStatus_SUCCESS;
}

template <CDataType TARGET_TYPE>
size_t BooleanArrayWarpSqlAccessor<TARGET_TYPE>::GetCellLength_impl(ColumnBinding *binding) const {
  return sizeof(unsigned char);
}

template class BooleanArrayWarpSqlAccessor<odbcabstraction::CDataType_BIT>;

} // namespace warpsql
} // namespace driver
