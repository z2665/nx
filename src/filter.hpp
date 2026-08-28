// filter.hpp：压缩过滤器引擎（设计 §3.1）
// zlib/bzip2/xz/lzma/zstd/lz4 直连原生库；compress(.Z) 走 libarchive。
// 关键：gzip/bz2/xz/zstd/lz4 允许多成员串联，成员结束时窥探后续 magic 决定重启解码器。
#pragma once
#include "bytesource.hpp"
#include "format.hpp"
#include "pipes.hpp"
#include <atomic>
#include <exception>
#include <functional>

namespace nx {

struct FilterStats {
    std::atomic<uint64_t>* produced = nullptr;   // 产出计数（压缩比熔断用）
};

// 解码泵主体：从 in 顺序读压缩流，解码后以块推入 out。
// stop=true 时尽快退出（下游已放弃）。异常在泵线程内捕获后经 err 传出。
// 由 walker 在独立线程中调用（D4：每 FilterStage 一个线程）。
void filter_decode(Format fmt, PushbackSource& in, BoundedQueue<std::vector<byte>>& out,
                   std::exception_ptr& err);

// .Z（compress）经由 libarchive raw+filter 解码（单成员）
void decode_via_libarchive(PushbackSource& in,
                           const std::function<bool(std::span<const byte>)>& emit,
                           Format filterAs);

} // namespace nx
