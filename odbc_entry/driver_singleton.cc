/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#include "driver_singleton.h"

#include <mutex>

namespace hardcharger {
namespace odbc {

static std::once_flag s_initFlag;
static std::shared_ptr<driver::hardcharger::HardchargerDriver> s_driver;

std::shared_ptr<driver::hardcharger::HardchargerDriver> GetGlobalDriver() {
  std::call_once(s_initFlag, []() {
    s_driver = std::make_shared<driver::hardcharger::HardchargerDriver>();
    s_driver->RegisterLog();
  });
  return s_driver;
}

} // namespace odbc
} // namespace hardcharger
