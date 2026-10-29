/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include "arrow/type_fwd.h"
#include "types.h"
#include <odbcabstraction/types.h>
#include <odbcabstraction/diagnostics.h>

namespace driver {
namespace chargewire_odbc {

using namespace arrow;
using namespace odbcabstraction;

template <CDataType TARGET_TYPE>
class BooleanArrayChargewireAccessor
    : public ChargewireAccessor<BooleanArray, TARGET_TYPE,
                               BooleanArrayChargewireAccessor<TARGET_TYPE>> {
public:
  explicit BooleanArrayChargewireAccessor(Array *array);

  RowStatus MoveSingleCell_impl(ColumnBinding *binding, int64_t arrow_row,
                                int64_t i, int64_t &value_offset,
                                bool update_value_offset,
                                odbcabstraction::Diagnostics &diagnostics);

  size_t GetCellLength_impl(ColumnBinding *binding) const;
};

} // namespace chargewire
} // namespace driver
