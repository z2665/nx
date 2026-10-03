// spool.hpp：SpoolStore——R 类容器的 seek 适配器（设计 §4：RAM 优先 → 磁盘溢出）
// · append 随上游解码推进（溢出写入与上游解压并发——流水线收益保住的那一半）；
//   下游随机访问在 finish() 之后（R 类引擎开卷本就需完整数据）
// · 溢出卷 = res::TempFile（FILE_FLAG_DELETE_ON_CLOSE：句柄一关内核即删，
//   清理不依赖对象生命周期——15GB spool 残留案例的教训）；RAM 全量落盘
//   分块 ≤16MiB（8GiB 环整段 cast DWORD 截断成 0 的教训，案例 M）
// · RAM 上限 ramCap 由调用方注入（自动策略：空闲物理内存 50%，64MiB–8GiB，
//   装配单点 = walker.cpp resolve_runtime_options）
// · ioM_ 串行化句柄访问——read_at 支持多线程并发（SeekView 契约，views.hpp）
#pragma once
#include "bytesource.hpp"
#include "res/temp_file.hpp"
#include <atomic>
#include <mutex>

namespace nx {

class SpoolBuffer : public std::enable_shared_from_this<SpoolBuffer> {
public:
    // ramCap：RAM 驻留上限；tempDir：溢出目录（空 = 系统临时目录）
    SpoolBuffer(size_t ramCap, const std::wstring& tempDir);
    ~SpoolBuffer();

    SpoolBuffer(const SpoolBuffer&) = delete;
    SpoolBuffer& operator=(const SpoolBuffer&) = delete;

    void append(std::span<const byte> p);
    void finish();
    uint64_t size() const { return total_; }
    // 随机访问读（szcom 引擎的 spool 卷视图用）
    size_t read_at(uint64_t pos, std::span<byte> buf) { return readAt(pos, buf); }
    bool overflowed() const { return overflowed_; }
    uint64_t tempBytes() const { return overflowed_ ? total_ : 0; }

    // 独立随机访问读视图已删（F3 死代码：全仓零调用，引擎侧统一走 SeekView 体系）

    // 子窗口视图：[start, start+len) 的独立顺序流（条目级分片组暂存用）——
    // 顺序流形态；随机访问形态 = views.hpp SpoolWindowView（同一区间两类消费方）
    class Window : public ByteSource {
    public:
        Window(std::shared_ptr<SpoolBuffer> s, uint64_t start, uint64_t len)
            : s_(std::move(s)), start_(start), len_(len) {
            if (start + len > s_->size()) throw Error("spool 窗口越界");
        }
        size_t read(std::span<byte> buf) override {
            if (pos_ >= len_) return 0;
            uint64_t avail = len_ - pos_;
            size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), avail));
            size_t got = s_->readAt(start_ + pos_, std::span<byte>(buf.data(), n));
            pos_ += got;
            return got;
        }
        std::optional<uint64_t> sizeHint() const override { return len_; }
    private:
        std::shared_ptr<SpoolBuffer> s_;
        uint64_t start_, len_, pos_ = 0;
    };

private:
    void flushToTemp();
    size_t readAt(uint64_t pos, std::span<byte> buf);

    size_t ramCap_;
    std::wstring tempDir_;
    std::vector<byte> ram_;
    res::TempFile tmp_;   // P2 圈禁（批次 5）：溢出卷唯一工厂（DELETE_ON_CLOSE）
    uint64_t total_ = 0;
    bool overflowed_ = false;
    bool finished_ = false;
    std::mutex ioM_;                // 串行化文件句柄访问
};

} // namespace nx
