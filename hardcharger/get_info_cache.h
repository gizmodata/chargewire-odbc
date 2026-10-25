/*
 * Copyright (C) 2020-2022 Dremio Corporation
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include "warp_client.h"
#include <atomic>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <odbcabstraction/spi/connection.h>

namespace driver {
namespace hardcharger {

class GetInfoCache {

private:
  std::unordered_map<uint16_t, driver::odbcabstraction::Connection::Info> info_;
  CallOptions &call_options_;
  std::unique_ptr<WarpSqlClient> &sql_client_;
  std::mutex mutex_;
  std::atomic<bool> has_server_info_;

public:
  GetInfoCache(CallOptions &call_options,
               std::unique_ptr<WarpSqlClient> &client,
               const std::string &driver_version);
  void SetProperty(uint16_t property,
                   driver::odbcabstraction::Connection::Info value);
  driver::odbcabstraction::Connection::Info GetInfo(uint16_t info_type);

private:
  bool LoadInfoFromServer();
  void LoadDefaultsForMissingEntries();
};
} // namespace hardcharger
} // namespace driver
