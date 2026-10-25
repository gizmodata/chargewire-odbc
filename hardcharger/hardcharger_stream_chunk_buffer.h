// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
#pragma once
#include "warp_client.h"
namespace driver::hardcharger {
class StreamChunkBuffer {
  std::shared_ptr<ResultInfo> info_;
public:
  StreamChunkBuffer(WarpSqlClient&, const CallOptions&, const std::shared_ptr<ResultInfo>& info,
                         size_t = 5, bool = false) : info_(info) {}
  ~StreamChunkBuffer() { try { Close(); } catch (...) {} }
  void Close() { if (info_) { info_->Close(); info_.reset(); } }
  bool GetNext(StreamChunk* chunk) {
    if (!info_ || !info_->reader) return false;
    ThrowWarpErrors([&] { auto s = info_->reader->ReadNext(&chunk->data);
      if (!s.ok()) throw odbcabstraction::DriverException(s.message()); });
    return chunk->data != nullptr;
  }
};
}
