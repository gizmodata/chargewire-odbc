/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include <memory>
#include <hardcharger/hardcharger_driver.h>

namespace hardcharger {
namespace odbc {

/// Returns the global HardchargerDriver singleton, creating it on first call.
/// Thread-safe via std::call_once.
std::shared_ptr<driver::hardcharger::HardchargerDriver> GetGlobalDriver();

} // namespace odbc
} // namespace hardcharger
