/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include <arrow/record_batch.h>
#include <arrow/type.h>
#include <memory>
#include <odbcabstraction/types.h>

namespace driver {
namespace chargewire_odbc {

/// Converts resolved ODBC parameter sets into the Arrow parameter body of a
/// Chargewire prepared statement: one row per parameter set, typed by
/// param_schema (an untyped placeholder, Arrow null, takes its type from the
/// bound C type). A value that cannot be converted exactly throws
/// ParameterRowException with the set's position in the application's array.
std::shared_ptr<arrow::RecordBatch> BuildParameterBatch(
    const std::shared_ptr<arrow::Schema>& param_schema,
    const odbcabstraction::ParameterSet& parameters);

} // namespace chargewire_odbc
} // namespace driver
