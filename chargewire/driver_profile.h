// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
#pragma once
#include <odbcabstraction/spi/connection.h>
#include "chargewire_client.h"

namespace driver::chargewire_odbc {
using ConnectionProperties = odbcabstraction::Connection::ConnPropertyMap;
// A product wrapper supplies only these hooks. Transport, ODBC behavior and
// regression suites remain in the generic Chargewire driver.
struct DriverProfile {
  const char* name;
  const char* product;
  const char* user_agent;
  const char* config_file;
  int default_port;
  // Optional: the port for a host given without one (the host/port properties of a DSN or
  // connection string). Null means default_port for every host. A wrapper sets it to give
  // some hosts another default; an explicit port always wins.
  int (*default_port_for_host)(const std::string& host) = nullptr;
};
const DriverProfile& GetDriverProfile();
// The port used when no port is given for host: profile.default_port_for_host(host) when the
// profile sets it, otherwise profile.default_port.
int DefaultPortForHost(const DriverProfile& profile, const std::string& host);
std::string TranslateProductUri(const std::string& uri);
void InitializeProductSession(ChargewireClient& client, const ConnectionProperties& properties,
                              const CallOptions& options);
// URI values supply connection defaults; explicit ODBC properties override them.
ConnectionProperties NormalizeConnectionProperties(const ConnectionProperties& properties);
// A URI written before the protocol's rename: its old scheme becomes the current one; any
// other URI is returned unchanged (pre_rename_compat.cc). The client library itself maps
// the old mount path and falls back to servers from before the rename.
std::string CurrentUriSpelling(const std::string& uri);
// The trace switch's name from before the rename, still read when the current one is unset
// (pre_rename_compat.cc).
const char* PreRenameTraceVariable();
}
