// filter.hpp：压缩过滤器引擎（设计 §3.1）
// zlib/bzip2/xz/lzma/zstd/lz4 直连原生库；compress(.Z) 走 libarchive。
// 关键：gzip/bz2/xz/zstd/lz4 允许多成员串联，成员结束时窥探后续 magic 决定重启解码器。
#pragma once
#include "bytesource.hpp"
#include "pushback.hpp"
#include "format.hpp"
#include "pipes.hpp"
#include <atomic>
#include <exception>
#include <functional>

namespace nx {

// 压缩比熔断（D6）：produced/inputBytes > maxRatio → LimitError（泵内抛出经 err 传回）
struct FilterLimiter {
    std::atomic<uint64_t>* produced = nullptr;         // 过滤器累计产出（分子）
    const std::atomic<uint64_t>* inputBytes = nullptr; // 根输入累计（分母，实时）
    uint64_t inputFloor = 0;                           // 根尺寸提示（小输入炸弹也判）
    uint64_t maxRatio = 0;                             // 0 = 不检查
    std::atomic<bool>* limitTripped = nullptr;
};

// 解码泵主体：从 in 顺序读压缩流，解码后以块推入 out。
// 异常在泵线程内捕获后经 err 传出。
// 由 walker 在独立线程中调用（D4：每 FilterStage 一个线程）。
void filter_decode(Format fmt, PushbackSource& in, BoundedQueue<std::vector<byte>>& out,
                   std::exception_ptr& err, const FilterLimiter& lim = FilterLimiter{});

// .Z（compress）经由 libarchive raw+filter 解码（单成员）
void decode_via_libarchive(PushbackSource& in,
                           const std::function<bool(std::span<const byte>)>& emit,
                           Format filterAs);

} // namespace nx
