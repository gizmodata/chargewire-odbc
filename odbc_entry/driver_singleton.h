/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include <memory>
#include <chargewire/chargewire_driver.h>

namespace chargewire_odbc {
namespace odbc {

/// Returns the global ChargewireDriver singleton, creating it on first call.
/// Thread-safe via std::call_once.
std::shared_ptr<driver::chargewire_odbc::ChargewireDriver> GetGlobalDriver();

} // namespace odbc
} // namespace chargewire
