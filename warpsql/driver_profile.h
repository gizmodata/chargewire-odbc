// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
#pragma once
#include <odbcabstraction/spi/connection.h>
#include "warp_client.h"

namespace driver::warpsql_odbc {
using ConnectionProperties = odbcabstraction::Connection::ConnPropertyMap;
// A product wrapper supplies only these hooks. Transport, ODBC behavior and
// regression suites remain in the generic Warp SQL driver.
struct DriverProfile {
  const char* name;
  const char* product;
  const char* user_agent;
  const char* config_file;
  int default_port;
};
const DriverProfile& GetDriverProfile();
std::string TranslateProductUri(const std::string& uri);
void InitializeProductSession(WarpSqlClient& client, const ConnectionProperties& properties,
                              const CallOptions& options);
// URI values supply connection defaults; explicit ODBC properties override them.
ConnectionProperties NormalizeConnectionProperties(const ConnectionProperties& properties);
}
