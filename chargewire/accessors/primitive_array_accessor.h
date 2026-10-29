/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include "../chargewire_result_set.h"
#include "common.h"
#include "types.h"
#include <arrow/array.h>
#include <arrow/scalar.h>
#include <odbcabstraction/types.h>

namespace driver {
namespace chargewire_odbc {

using namespace arrow;
using namespace odbcabstraction;

template <typename ARROW_ARRAY, CDataType TARGET_TYPE>
class PrimitiveArrayChargewireAccessor
    : public ChargewireAccessor<
          ARROW_ARRAY, TARGET_TYPE,
          PrimitiveArrayChargewireAccessor<ARROW_ARRAY, TARGET_TYPE>> {
public:
  explicit PrimitiveArrayChargewireAccessor(Array *array);

  size_t GetColumnarData_impl(ColumnBinding *binding, int64_t starting_row, int64_t cells,
                              int64_t &value_offset, bool update_value_offset,
                              odbcabstraction::Diagnostics &diagnostics, uint16_t* row_status_array);

  size_t GetCellLength_impl(ColumnBinding *binding) const;
};

} // namespace chargewire
} // namespace driver
