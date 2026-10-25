/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include <memory>
#include <warpsql/warpsql_driver.h>

namespace warpsql_odbc {
namespace odbc {

/// Returns the global WarpSqlDriver singleton, creating it on first call.
/// Thread-safe via std::call_once.
std::shared_ptr<driver::warpsql_odbc::WarpSqlDriver> GetGlobalDriver();

} // namespace odbc
} // namespace warpsql
