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

namespace driver {
namespace hardcharger {

using namespace arrow;
using namespace odbcabstraction;

Accessor* CreateTimeAccessor(arrow::Array *array, arrow::Type::type type);

template <CDataType TARGET_TYPE, typename ARROW_ARRAY, arrow::TimeUnit::type UNIT>
class TimeArrayHardchargerAccessor
    : public HardchargerAccessor<
          ARROW_ARRAY, TARGET_TYPE,
          TimeArrayHardchargerAccessor<TARGET_TYPE, ARROW_ARRAY, UNIT>> {

public:
  explicit TimeArrayHardchargerAccessor(Array *array);

  RowStatus MoveSingleCell_impl(ColumnBinding *binding, int64_t arrow_row, int64_t cell_counter,
                           int64_t &value_offset, bool update_value_offset,
                           odbcabstraction::Diagnostics &diagnostic);

  size_t GetCellLength_impl(ColumnBinding *binding) const;
};
} // namespace hardcharger
} // namespace driver
